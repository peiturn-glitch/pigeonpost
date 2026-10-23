#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace brevduva::mime {

void init();

struct Address {
    std::string name;
    std::string email;
};

struct HeaderSummary {
    std::string subject;
    std::vector<Address> from;
    std::vector<Address> to;
    std::int64_t date = 0;  // Unix-sekunder.
    bool hasAttachments = false;
    std::string messageId;  // Utan vinkelparenteser. Tom om huvudet saknas eller inte hämtades.
};

// Tolkar ett block med headerfält (från BODY.PEEK[HEADER.FIELDS ...]).
HeaderSummary parseHeaders(const std::string& raw, const std::string& internalDate);
// Tolkar ett datum i e-postformat (Date-huvud eller IMAP INTERNALDATE) till unixtid. 0 om det inte går.
std::int64_t parseDate(const std::string& text);

struct Attachment {
    int index = 0;
    std::string filename;
    std::string contentType;
    std::string contentId;
    bool inlineInHtml = false;  // Inbäddad bild som redan visas i HTML-delen.
    std::string data;
};

struct ParsedMessage {
    std::string subject;
    std::vector<Address> from, to, cc, replyTo;
    std::int64_t date = 0;
    std::string messageId;
    std::string references;
    std::string text;
    std::string html;
    std::vector<Attachment> attachments;
};

// embedInlineImages = true ersätter cid:-referenser i HTML-delen med data-URI:er. Med false lämnas de orörda
// (bilderna hämtas då som bilagor), men inlineInHtml sätts ändå för bilder som HTML-delen pekar på.
ParsedMessage parseMessage(const std::string& raw, bool embedInlineImages = true);

struct OutgoingAttachment {
    std::string filename;
    std::string contentType;
    std::string data;
};

struct OutgoingMessage {
    std::string fromName;
    std::string fromEmail;
    std::string replyTo;
    std::string to, cc, bcc;  // Kommaseparerade adresslistor.
    std::string subject;
    std::string text;
    // Valfri HTML-version (innehållet i <body>). Skickas som multipart/alternative tillsammans med text.
    // Bilder med data-URI (<img src="data:image/png;base64,…">) bäddas in som egna delar med cid-referenser.
    std::string html;
    std::string inReplyTo;
    std::string references;
    std::vector<OutgoingAttachment> attachments;
};

struct BuiltMessage {
    std::string data;  // CRLF-radslut.
    std::vector<std::string> recipients;
};

BuiltMessage buildMessage(const OutgoingMessage& msg);

}  // namespace brevduva::mime
