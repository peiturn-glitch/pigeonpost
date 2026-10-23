#include "mime.hpp"

#include "crypto.hpp"
#include "errors.hpp"

#include <gmime/gmime.h>

#include <algorithm>
#include <cctype>
#include <memory>
#include <set>

namespace brevduva::mime {

namespace {

constexpr std::size_t kMaxInlineImage = 5 * 1024 * 1024;

struct GObjectDeleter {
    void operator()(gpointer p) const {
        if (p) g_object_unref(p);
    }
};
template <typename T>
using GPtr = std::unique_ptr<T, GObjectDeleter>;

std::string safe(const char* s) {
    return s ? std::string(s) : std::string();
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

GPtr<GMimeMessage> parse(const std::string& raw) {
    GPtr<GMimeStream> stream(g_mime_stream_mem_new_with_buffer(raw.data(), raw.size()));
    GPtr<GMimeParser> parser(g_mime_parser_new_with_stream(stream.get()));
    return GPtr<GMimeMessage>(g_mime_parser_construct_message(parser.get(), nullptr));
}

void collectAddresses(InternetAddressList* list, std::vector<Address>& out) {
    if (!list) return;
    int n = internet_address_list_length(list);
    for (int i = 0; i < n; ++i) {
        InternetAddress* ia = internet_address_list_get_address(list, i);
        if (INTERNET_ADDRESS_IS_MAILBOX(ia)) {
            out.push_back({safe(internet_address_get_name(ia)),
                           safe(internet_address_mailbox_get_addr(INTERNET_ADDRESS_MAILBOX(ia)))});
        } else if (INTERNET_ADDRESS_IS_GROUP(ia)) {
            collectAddresses(internet_address_group_get_members(INTERNET_ADDRESS_GROUP(ia)), out);
        }
    }
}

std::int64_t messageDate(GMimeMessage* msg, const std::string& fallback) {
    if (GDateTime* dt = g_mime_message_get_date(msg)) return g_date_time_to_unix(dt);
    if (!fallback.empty()) {
        if (GDateTime* dt = g_mime_utils_header_decode_date(fallback.c_str())) {
            std::int64_t t = g_date_time_to_unix(dt);
            g_date_time_unref(dt);
            return t;
        }
    }
    return 0;
}

// Avkodar transfer-encoding och, för text, konverterar teckenkodning till UTF-8.
std::string partContent(GMimePart* part, bool asText) {
    GMimeDataWrapper* wrapper = g_mime_part_get_content(part);
    if (!wrapper) return {};

    GPtr<GMimeStream> mem(g_mime_stream_mem_new());
    GPtr<GMimeStream> filtered(g_mime_stream_filter_new(mem.get()));
    if (asText) {
        const char* charset = g_mime_object_get_content_type_parameter(GMIME_OBJECT(part), "charset");
        std::string cs = charset ? lower(charset) : "";
        if (!cs.empty() && cs != "utf-8" && cs != "us-ascii") {
            if (GMimeFilter* f = g_mime_filter_charset_new(charset, "UTF-8")) {
                g_mime_stream_filter_add(GMIME_STREAM_FILTER(filtered.get()), f);
                g_object_unref(f);
            }
        }
    }
    g_mime_data_wrapper_write_to_stream(wrapper, filtered.get());
    g_mime_stream_flush(filtered.get());
    GByteArray* bytes = g_mime_stream_mem_get_byte_array(GMIME_STREAM_MEM(mem.get()));
    return std::string(reinterpret_cast<const char*>(bytes->data), bytes->len);
}

std::string mimeType(GMimeObject* obj) {
    GMimeContentType* ct = g_mime_object_get_content_type(obj);
    if (!ct) return "application/octet-stream";
    char* t = g_mime_content_type_get_mime_type(ct);
    std::string out = lower(safe(t));
    g_free(t);
    return out;
}

std::string serialize(GMimeObject* obj) {
    GMimeFormatOptions* opts = g_mime_format_options_new();
    g_mime_format_options_set_newline_format(opts, GMIME_NEWLINE_FORMAT_DOS);
    char* s = g_mime_object_to_string(obj, opts);
    g_mime_format_options_free(opts);
    std::string out = safe(s);
    g_free(s);
    return out;
}

void walk(GMimeObject* obj, ParsedMessage& out) {
    if (GMIME_IS_MULTIPART(obj)) {
        GMimeMultipart* mp = GMIME_MULTIPART(obj);
        int n = g_mime_multipart_get_count(mp);
        for (int i = 0; i < n; ++i) walk(g_mime_multipart_get_part(mp, i), out);
        return;
    }

    if (GMIME_IS_MESSAGE_PART(obj)) {
        GMimeMessage* inner = g_mime_message_part_get_message(GMIME_MESSAGE_PART(obj));
        Attachment a;
        a.index = static_cast<int>(out.attachments.size());
        a.contentType = "message/rfc822";
        std::string subject = inner ? safe(g_mime_message_get_subject(inner)) : "";
        a.filename = (subject.empty() ? std::string("meddelande") : subject) + ".eml";
        if (inner) a.data = serialize(GMIME_OBJECT(inner));
        out.attachments.push_back(std::move(a));
        return;
    }

    if (!GMIME_IS_PART(obj)) return;
    GMimePart* part = GMIME_PART(obj);
    std::string type = mimeType(obj);
    bool isAttachment = g_mime_part_is_attachment(part);
    const char* filename = g_mime_part_get_filename(part);

    if (!isAttachment && !filename && (type == "text/plain" || type == "text/html")) {
        std::string content = partContent(part, true);
        std::string& target = type == "text/plain" ? out.text : out.html;
        if (!target.empty()) target += type == "text/plain" ? "\n\n" : "<hr>";
        target += content;
        return;
    }

    Attachment a;
    a.index = static_cast<int>(out.attachments.size());
    a.contentType = type;
    a.filename = safe(filename);
    a.contentId = safe(g_mime_object_get_content_id(obj));
    if (a.filename.empty()) {
        a.filename = "bilaga-" + std::to_string(a.index + 1);
    }
    a.data = partContent(part, false);
    out.attachments.push_back(std::move(a));
}

void replaceAll(std::string& s, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    std::size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}

std::vector<std::string> addressesOf(InternetAddressList* list) {
    std::vector<Address> tmp;
    collectAddresses(list, tmp);
    std::vector<std::string> out;
    for (auto& a : tmp) out.push_back(a.email);
    return out;
}

// Tolkar en adresslista från användaren och lägger den i meddelandets header.
void addAddresses(GMimeMessage* msg, GMimeAddressType type, const std::string& input,
                  std::vector<std::string>& recipients) {
    if (std::all_of(input.begin(), input.end(), [](unsigned char c) { return std::isspace(c) || c == ','; })) {
        return;
    }
    InternetAddressList* parsed = internet_address_list_parse(nullptr, input.c_str());
    if (!parsed) throw BadRequest("Ogiltig adress: " + input);
    GPtr<InternetAddressList> guard(parsed);
    for (const auto& addr : addressesOf(parsed)) {
        std::size_t at = addr.find('@');
        if (at == std::string::npos || at == 0 || at + 1 == addr.size()) {
            throw BadRequest("Ogiltig adress: " + addr);
        }
        recipients.push_back(addr);
    }
    if (msg) internet_address_list_append(g_mime_message_get_addresses(msg, type), parsed);
}

GMimeObject* textPart(const std::string& text) {
    GMimeTextPart* part = g_mime_text_part_new_with_subtype("plain");
    g_mime_text_part_set_text(part, text.c_str());
    g_mime_part_set_content_encoding(
        GMIME_PART(part), g_mime_part_get_best_content_encoding(GMIME_PART(part), GMIME_ENCODING_CONSTRAINT_7BIT));
    return GMIME_OBJECT(part);
}

struct InlineImage {
    std::string subtype;  // png, jpeg, gif eller webp
    std::string data;
    std::string contentId;
};

// Byter ut bilder med data-URI mot cid-referenser och returnerar bilderna. Många e-postprogram, bland annat
// Gmail, visar inte data-URI:er, men alla visar inbäddade delar i multipart/related.
std::vector<InlineImage> extractInlineImages(std::string& html, const std::string& domain) {
    static const std::set<std::string> allowed = {"png", "jpeg", "gif", "webp"};
    std::vector<InlineImage> images;
    const std::string marker = "src=\"data:image/";
    std::size_t pos = 0;
    while ((pos = html.find(marker, pos)) != std::string::npos) {
        std::size_t valueStart = pos + 5;  // Efter src="
        std::size_t valueEnd = html.find('"', valueStart);
        if (valueEnd == std::string::npos) break;
        std::string uri = html.substr(valueStart, valueEnd - valueStart);
        std::size_t semi = uri.find(';');
        std::size_t comma = uri.find(',');
        std::string subtype = semi == std::string::npos ? "" : lower(uri.substr(11, semi - 11));
        if (subtype == "jpg") subtype = "jpeg";
        if (comma == std::string::npos || semi > comma || uri.compare(semi, comma - semi, ";base64") != 0 ||
            !allowed.count(subtype)) {
            pos = valueEnd;
            continue;
        }
        InlineImage image;
        image.subtype = subtype;
        image.data = crypto::base64Decode(uri.substr(comma + 1));
        image.contentId = "bild" + std::to_string(images.size() + 1) + "." + crypto::toHex(crypto::randomBytes(6)) + "@" + domain;
        std::string ref = "cid:" + image.contentId;
        html.replace(valueStart, valueEnd - valueStart, ref);
        pos = valueStart + ref.size();
        images.push_back(std::move(image));
    }
    return images;
}

GMimeObject* htmlPart(const std::string& body) {
    std::string html = "<!DOCTYPE html>\n<html><head><meta http-equiv=\"Content-Type\" content=\"text/html; charset=utf-8\">"
                       "</head><body>\n" + body + "\n</body></html>\n";
    GMimeTextPart* part = g_mime_text_part_new_with_subtype("html");
    g_mime_text_part_set_text(part, html.c_str());
    g_mime_part_set_content_encoding(
        GMIME_PART(part), g_mime_part_get_best_content_encoding(GMIME_PART(part), GMIME_ENCODING_CONSTRAINT_7BIT));
    return GMIME_OBJECT(part);
}

GMimeObject* inlineImagePart(const InlineImage& image, std::size_t number) {
    GMimePart* part = g_mime_part_new_with_type("image", image.subtype.c_str());
    GPtr<GMimeStream> stream(g_mime_stream_mem_new_with_buffer(image.data.data(), image.data.size()));
    GPtr<GMimeDataWrapper> wrapper(g_mime_data_wrapper_new_with_stream(stream.get(), GMIME_CONTENT_ENCODING_DEFAULT));
    g_mime_part_set_content(part, wrapper.get());
    g_mime_object_set_disposition(GMIME_OBJECT(part), GMIME_DISPOSITION_INLINE);
    std::string filename = "bild" + std::to_string(number) + "." + (image.subtype == "jpeg" ? "jpg" : image.subtype);
    g_mime_part_set_filename(part, filename.c_str());
    g_mime_object_set_content_id(GMIME_OBJECT(part), image.contentId.c_str());
    g_mime_part_set_content_encoding(part, GMIME_CONTENT_ENCODING_BASE64);
    return GMIME_OBJECT(part);
}

// Meddelandets innehåll: ren text, eller multipart/alternative med text och HTML (med inbäddade bilder).
GMimeObject* bodyPart(const OutgoingMessage& in, const std::string& domain) {
    if (in.html.empty()) return textPart(in.text);

    std::string html = in.html;
    auto images = extractInlineImages(html, domain);

    GMimeMultipart* alternative = g_mime_multipart_new_with_subtype("alternative");
    GMimeObject* text = textPart(in.text);
    g_mime_multipart_add(alternative, text);
    g_object_unref(text);

    GMimeObject* rich = htmlPart(html);
    if (!images.empty()) {
        GMimeMultipart* related = g_mime_multipart_new_with_subtype("related");
        g_mime_object_set_content_type_parameter(GMIME_OBJECT(related), "type", "text/html");
        g_mime_multipart_add(related, rich);
        g_object_unref(rich);
        for (std::size_t i = 0; i < images.size(); ++i) {
            GMimeObject* part = inlineImagePart(images[i], i + 1);
            g_mime_multipart_add(related, part);
            g_object_unref(part);
        }
        rich = GMIME_OBJECT(related);
    }
    g_mime_multipart_add(alternative, rich);
    g_object_unref(rich);
    return GMIME_OBJECT(alternative);
}

GMimeObject* attachmentPart(const OutgoingAttachment& att) {
    std::string type = att.contentType.empty() ? "application/octet-stream" : att.contentType;
    std::size_t slash = type.find('/');
    std::string major = slash == std::string::npos ? "application" : type.substr(0, slash);
    std::string minor = slash == std::string::npos ? "octet-stream" : type.substr(slash + 1);
    std::size_t semi = minor.find(';');
    if (semi != std::string::npos) minor = minor.substr(0, semi);

    GMimePart* part = g_mime_part_new_with_type(major.c_str(), minor.c_str());
    GPtr<GMimeStream> stream(g_mime_stream_mem_new_with_buffer(att.data.data(), att.data.size()));
    GPtr<GMimeDataWrapper> wrapper(g_mime_data_wrapper_new_with_stream(stream.get(), GMIME_CONTENT_ENCODING_DEFAULT));
    g_mime_part_set_content(part, wrapper.get());
    g_mime_object_set_disposition(GMIME_OBJECT(part), GMIME_DISPOSITION_ATTACHMENT);
    g_mime_part_set_filename(part, att.filename.empty() ? "bilaga" : att.filename.c_str());
    g_mime_part_set_content_encoding(part, GMIME_CONTENT_ENCODING_BASE64);
    return GMIME_OBJECT(part);
}

}  // namespace

void init() {
    g_mime_init();
}

HeaderSummary parseHeaders(const std::string& raw, const std::string& internalDate) {
    HeaderSummary h;
    // Säkerställ att headerblocket avslutas med en tom rad så att parsern inte väntar på brödtext.
    std::string block = raw;
    if (block.find("\r\n\r\n") == std::string::npos && block.find("\n\n") == std::string::npos) {
        block += "\r\n";
    }
    auto msg = parse(block);
    if (!msg) return h;
    h.subject = safe(g_mime_message_get_subject(msg.get()));
    collectAddresses(g_mime_message_get_from(msg.get()), h.from);
    collectAddresses(g_mime_message_get_addresses(msg.get(), GMIME_ADDRESS_TYPE_TO), h.to);
    h.date = messageDate(msg.get(), internalDate);
    h.hasAttachments = lower(raw).find("multipart/mixed") != std::string::npos;
    h.messageId = safe(g_mime_message_get_message_id(msg.get()));
    return h;
}

std::int64_t parseDate(const std::string& text) {
    if (text.empty()) return 0;
    GDateTime* dt = g_mime_utils_header_decode_date(text.c_str());
    if (!dt) return 0;
    std::int64_t t = g_date_time_to_unix(dt);
    g_date_time_unref(dt);
    return t;
}

ParsedMessage parseMessage(const std::string& raw, bool embedInlineImages) {
    ParsedMessage out;
    auto msg = parse(raw);
    if (!msg) throw MailServerError("Meddelandet kunde inte tolkas");

    out.subject = safe(g_mime_message_get_subject(msg.get()));
    collectAddresses(g_mime_message_get_from(msg.get()), out.from);
    collectAddresses(g_mime_message_get_addresses(msg.get(), GMIME_ADDRESS_TYPE_TO), out.to);
    collectAddresses(g_mime_message_get_addresses(msg.get(), GMIME_ADDRESS_TYPE_CC), out.cc);
    collectAddresses(g_mime_message_get_reply_to(msg.get()), out.replyTo);
    out.date = messageDate(msg.get(), "");
    out.messageId = safe(g_mime_message_get_message_id(msg.get()));
    out.references = safe(g_mime_object_get_header(GMIME_OBJECT(msg.get()), "References"));

    if (GMimeObject* body = g_mime_message_get_mime_part(msg.get())) walk(body, out);

    // Ersätt cid:-referenser med data-URI:er så att inbäddade bilder visas utan extra anrop.
    if (!out.html.empty()) {
        std::string lowerHtml = lower(out.html);
        for (auto& a : out.attachments) {
            if (a.contentId.empty() || a.contentType.rfind("image/", 0) != 0 || a.data.size() > kMaxInlineImage) {
                continue;
            }
            std::string ref = "cid:" + a.contentId;
            if (lowerHtml.find(lower(ref)) == std::string::npos) continue;
            if (!embedInlineImages) {
                a.inlineInHtml = true;
                continue;
            }
            std::string uri = "data:" + a.contentType + ";base64," + crypto::base64Encode(a.data);
            replaceAll(out.html, ref, uri);
            a.inlineInHtml = true;
        }
    }
    return out;
}

BuiltMessage buildMessage(const OutgoingMessage& in) {
    BuiltMessage built;
    GPtr<GMimeMessage> msg(g_mime_message_new(TRUE));

    g_mime_message_add_mailbox(msg.get(), GMIME_ADDRESS_TYPE_FROM, in.fromName.c_str(), in.fromEmail.c_str());
    addAddresses(msg.get(), GMIME_ADDRESS_TYPE_TO, in.to, built.recipients);
    addAddresses(msg.get(), GMIME_ADDRESS_TYPE_CC, in.cc, built.recipients);
    // Bcc skrivs inte in i headern, bara i SMTP-kuvertet.
    addAddresses(nullptr, GMIME_ADDRESS_TYPE_BCC, in.bcc, built.recipients);
    std::vector<std::string> ignored;  // Svarsadressen är inte en mottagare.
    addAddresses(msg.get(), GMIME_ADDRESS_TYPE_REPLY_TO, in.replyTo, ignored);

    g_mime_message_set_subject(msg.get(), in.subject.c_str(), "utf-8");
    GDateTime* now = g_date_time_new_now_local();
    g_mime_message_set_date(msg.get(), now);
    g_date_time_unref(now);

    std::string domain = "localhost";
    std::size_t at = in.fromEmail.find('@');
    if (at != std::string::npos) domain = in.fromEmail.substr(at + 1);
    char* id = g_mime_utils_generate_message_id(domain.c_str());
    g_mime_message_set_message_id(msg.get(), id);
    g_free(id);

    if (!in.inReplyTo.empty()) {
        g_mime_object_set_header(GMIME_OBJECT(msg.get()), "In-Reply-To", in.inReplyTo.c_str(), nullptr);
    }
    if (!in.references.empty()) {
        g_mime_object_set_header(GMIME_OBJECT(msg.get()), "References", in.references.c_str(), nullptr);
    }
    g_mime_object_set_header(GMIME_OBJECT(msg.get()), "User-Agent", "PigeonPost/0.1", nullptr);

    GMimeObject* text = bodyPart(in, domain);
    if (in.attachments.empty()) {
        g_mime_message_set_mime_part(msg.get(), text);
        g_object_unref(text);
    } else {
        GMimeMultipart* mixed = g_mime_multipart_new_with_subtype("mixed");
        g_mime_multipart_add(mixed, text);
        g_object_unref(text);
        for (const auto& att : in.attachments) {
            GMimeObject* part = attachmentPart(att);
            g_mime_multipart_add(mixed, part);
            g_object_unref(part);
        }
        g_mime_message_set_mime_part(msg.get(), GMIME_OBJECT(mixed));
        g_object_unref(mixed);
    }

    built.data = serialize(GMIME_OBJECT(msg.get()));
    return built;
}

}  // namespace brevduva::mime
