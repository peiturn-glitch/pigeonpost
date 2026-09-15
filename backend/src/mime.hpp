#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tbk::mime {

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
};

// Tolkar ett block med headerfält (från BODY.PEEK[HEADER.FIELDS ...]).
HeaderSummary parseHeaders(const std::string& raw, const std::string& internalDate);

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

ParsedMessage parseMessage(const std::string& raw);

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
    std::string inReplyTo;
    std::string references;
    std::vector<OutgoingAttachment> attachments;
};

struct BuiltMessage {
    std::string data;  // CRLF-radslut.
    std::vector<std::string> recipients;
};

BuiltMessage buildMessage(const OutgoingMessage& msg);

}  // namespace tbk::mime
