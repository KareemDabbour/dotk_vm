#include "typer.hpp"
#include "../vendor/json.hpp"
#include <algorithm>
#include <charconv>
#include <cctype>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <limits>
#include <optional>
#include <unordered_map>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {
using Json = nlohmann::json;
constexpr size_t maxMessage = 16 * 1024 * 1024;
constexpr size_t maxDocument = 4 * 1024 * 1024;
constexpr size_t maxDocuments = 1000;
constexpr size_t maxText = 32 * 1024 * 1024;

void send(const Json &message) {
    std::string body = message.dump();
    std::cout << "Content-Length: " << body.size() << "\r\n\r\n" << body; std::cout.flush();
}
void result(const Json &id, const Json &value) { send({{"jsonrpc", "2.0"}, {"id", id}, {"result", value}}); }
void failure(const Json &id, int code, const std::string &message) {
    send({{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}});
}
void log(const std::string &message) {
    send({{"jsonrpc", "2.0"}, {"method", "window/logMessage"}, {"params", {{"type", 1}, {"message", message}}}});
}
std::optional<std::string> readMessage() {
    std::string header; std::optional<size_t> length; size_t headers = 0;
    while (std::getline(std::cin, header)) {
        headers += header.size(); if (headers > 8192) throw std::runtime_error("LSP headers too large");
        if (!header.empty() && header.back() == '\r') header.pop_back();
        if (header.empty()) break;
        size_t colon = header.find(':'); if (colon == std::string::npos) throw std::runtime_error("Invalid LSP header");
        std::string key = header.substr(0, colon);
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char byte) { return static_cast<char>(std::tolower(byte)); });
        if (key != "content-length") continue;
        std::string value = header.substr(colon + 1); size_t start = value.find_first_not_of(" \t");
        if (start == std::string::npos || length) throw std::runtime_error("Invalid Content-Length");
        value = value.substr(start); size_t parsedLength = 0;
        auto parsed = std::from_chars(value.data(), value.data() + value.size(), parsedLength);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || parsedLength > maxMessage)
            throw std::runtime_error("Invalid Content-Length");
        length = parsedLength;
    }
    if (!std::cin && !headers) return {};
    if (!length || !std::cin) throw std::runtime_error("Truncated LSP headers");
    std::string body(*length, '\0'); std::cin.read(body.data(), static_cast<std::streamsize>(*length));
    if (static_cast<size_t>(std::cin.gcount()) != *length) throw std::runtime_error("Truncated LSP body");
    return body;
}
Json position(const std::shared_ptr<const dk3::Source> &source, size_t offset) {
    size_t line = 0, character = 0; offset = std::min(offset, source->text.size());
    auto offsets = dk3::utf8Offsets(source->text, {source, 0, 0, 1, 1});
    for (size_t index = 0; index + 1 < offsets.size() && offsets[index] < offset; index++) {
        size_t start = offsets[index]; char byte = source->text[start];
        if (byte == '\r') { line++; character = 0; }
        else if (byte == '\n') { if (!start || source->text[start - 1] != '\r') line++; character = 0; }
        else character += offsets[index + 1] - start == 4 ? 2 : 1;
    }
    return {{"line", line}, {"character", character}};
}
Json range(const std::shared_ptr<const dk3::Source> &source, size_t start, size_t end) {
    return {{"start", position(source, start)}, {"end", position(source, std::max(start, end))}};
}
size_t bytePosition(const std::shared_ptr<const dk3::Source> &source, const Json &position) {
    size_t wantedLine = position.at("line").get<size_t>(), wantedCharacter = position.at("character").get<size_t>();
    size_t line = 0, character = 0;
    auto offsets = dk3::utf8Offsets(source->text, {source, 0, 0, 1, 1});
    for (size_t index = 0; index < offsets.size(); index++) {
        size_t offset = offsets[index];
        if (line == wantedLine && character == wantedCharacter) return offset;
        if (index + 1 == offsets.size()) break;
        char byte = source->text[offset];
        if (byte == '\r') { line++; character = 0; }
        else if (byte == '\n') { if (!offset || source->text[offset - 1] != '\r') line++; character = 0; }
        else character += offsets[index + 1] - offset == 4 ? 2 : 1;
    }
    throw std::invalid_argument("Position is outside the document or splits a UTF-16 pair");
}
std::optional<std::filesystem::path> filePath(const std::string &uri) {
    if (uri.rfind("file:///", 0) != 0) return {};
    std::string decoded;
    for (size_t index = 7; index < uri.size(); index++) {
        if (uri[index] == '%') {
            if (index + 2 >= uri.size()) return {};
            unsigned value = 0; auto parsed = std::from_chars(uri.data() + index + 1, uri.data() + index + 3, value, 16);
            if (parsed.ec != std::errc{} || parsed.ptr != uri.data() + index + 3 || !value) return {};
            decoded += static_cast<char>(value); index += 2;
        } else decoded += uri[index];
    }
#ifdef _WIN32
    if (decoded.size() > 2 && decoded[0] == '/' && decoded[2] == ':') decoded.erase(0, 1);
#endif
    return std::filesystem::path(decoded).lexically_normal();
}
std::string signature(const dk3::Ast &node) {
    std::string text = node->kind;
    if (node->kind == "function" || node->kind == "signature" || node->kind == "record" || node->kind == "class") {
        text += " (";
        for (const auto &parameter : node->parameters) {
            if (text.back() != '(') text += ", ";
            if (parameter.rest) text += '*'; text += parameter.name;
            if (!parameter.type.empty()) text += ": " + parameter.type;
            if (parameter.defaultValue) text += " = ...";
        }
        text += ')';
    }
    if (node->kind == "variant") {
        text += " (";
        for (const auto &type : node->names) { if (text.back() != '(') text += ", "; text += type; }
        text += ')';
    }
    if (!node->type.empty()) text += " -> " + node->type;
    if ((node->kind == "class" || node->kind == "record") && !node->names.empty()) {
        text += " implements ";
        for (size_t index = 0; index < node->names.size(); index++) { if (index) text += ", "; text += node->names[index]; }
    }
    return text;
}
Json symbol(const dk3::Ast &node, const std::vector<dk3::Token> &tokens) {
    size_t start = 0;
    while (start < tokens.size() && tokens[start].span.offset < node->span.offset) start++;
    size_t selection = start;
    std::string selectionName = node->kind == "impl" ? node->text.substr(node->text.find_last_of('.') + 1) : node->text;
    while (selection < tokens.size() && !(tokens[selection].kind == dk3::TokenKind::Name && tokens[selection].text == selectionName)) selection++;
    if (selection == tokens.size()) return nullptr;
    size_t end = tokens[selection].span.offset + tokens[selection].span.length; int depth = 0;
    bool classHeader = node->kind == "class" || node->kind == "record";
    for (size_t index = start; index < tokens.size(); index++) {
        const auto &token = tokens[index];
        if (token.kind == dk3::TokenKind::End || (!classHeader && !depth && (token.kind == dk3::TokenKind::Newline || token.text == ";" || token.text == ","))) break;
        if (token.kind == dk3::TokenKind::Symbol && token.text == "{") classHeader = false;
        if (token.text == "(" || token.text == "[" || token.text == "{") depth++;
        if (token.text == ")" || token.text == "]" || token.text == "}") { if (!depth) break; depth--; }
        end = token.span.offset + token.span.length;
    }
    int kind = node->kind == "trait" ? 11 : node->kind == "impl" ? 19 : node->kind == "signature" ? 6 : node->kind == "class" ? 5 : node->kind == "record" ? 23 : node->kind == "enum" ? 10 : node->kind == "variant" ? 22 : node->kind == "function" ? 12 : node->mutableBinding ? 13 : 14;
    Json result = {{"name", node->text}, {"kind", kind}, {"detail", signature(node)},
        {"range", range(node->span.source, node->span.offset, end)},
        {"selectionRange", range(node->span.source, tokens[selection].span.offset, tokens[selection].span.offset + tokens[selection].span.length)}};
    if (node->kind == "enum") {
        result["children"] = Json::array();
        for (const auto &child : node->children) result["children"].push_back(symbol(child, tokens));
    }
    if (node->kind == "trait" || node->kind == "impl") {
        if (node->kind == "impl") result["name"] = "impl " + node->text + " for " + node->names[0];
        result["children"] = Json::array();
        for (const auto &method : node->children) {
            Json child = symbol(method, tokens);
            if (!child.is_null()) { child["kind"] = 6; result["children"].push_back(std::move(child)); }
        }
    }
    if (node->kind == "class" || node->kind == "record") {
        result["children"] = Json::array();
        for (const auto &field : node->parameters) for (size_t index = selection + 1; index < tokens.size() && tokens[index].span.offset < end; index++) {
            bool fieldToken = node->kind == "record" ? index + 1 < tokens.size() && tokens[index + 1].text == ":" : tokens[index - 1].text == "let" || tokens[index - 1].text == "var";
            if (tokens[index].text != field.name || !fieldToken) continue;
            auto location = range(node->span.source, tokens[index].span.offset, tokens[index].span.offset + tokens[index].span.length);
            result["children"].push_back({{"name", field.name}, {"kind", 8}, {"detail", (field.mutableBinding ? "var " : "let ") + field.type}, {"range", location}, {"selectionRange", location}});
            break;
        }
        for (const auto &method : node->children) {
            Json child = symbol(method, tokens);
            if (!child.is_null()) { child["kind"] = 6; result["children"].push_back(std::move(child)); }
        }
    }
    return result;
}
struct Document {
    int64_t version;
    std::shared_ptr<const dk3::Source> source;
    dk3::Ast tree;
    std::vector<dk3::Token> tokens;
};
int64_t versionNumber(const Json &value) {
    if (!value.is_number_integer() || (value.is_number_unsigned() && value.get<uint64_t>() > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())))
        throw std::invalid_argument("Document version must be an integer");
    return value.get<int64_t>();
}
class Server {
    std::unordered_map<std::string, Document> documents;
    bool initialized = false, shutdown = false;
    size_t textBytes = 0;
    // Source for an imported module: an open document wins over the file on disk.
    std::shared_ptr<const dk3::Source> moduleSource(const std::string &documentUri, const std::string &module) {
        auto parent = filePath(documentUri); if (!parent) return nullptr;
        std::string name = module;
        if (std::filesystem::path(name).is_absolute() || name.find("..") != std::string::npos) return nullptr;
        if (std::filesystem::path(name).extension() != ".k3") { std::replace(name.begin(), name.end(), '.', '/'); name += ".k3"; }
        auto path = (parent->parent_path() / name).lexically_normal();
        for (const auto &entry : documents) if (filePath(entry.first) == std::optional<std::filesystem::path>(path)) return entry.second.source;
        std::ifstream file(path, std::ios::binary); if (!file) return nullptr;
        std::string text(maxDocument + 1, '\0'); file.read(text.data(), static_cast<std::streamsize>(text.size()));
        size_t size = static_cast<size_t>(file.gcount()); if (size > maxDocument) return nullptr;
        text.resize(size);
        return std::make_shared<dk3::Source>(dk3::Source{path.string(), text});
    }
    void diagnostics(const std::string &uri, Document &document) {
        Json errors = Json::array(); document.tree.reset(); document.tokens.clear();
        try {
            document.tokens = dk3::lex(document.source); document.tree = dk3::parse(document.source);
            auto loader = [&](const std::string &name) { return moduleSource(uri, name); };
            for (const auto &found : dk3::typeCheck(document.tree, loader)) {
                if (found.span.source && found.span.source != document.source) continue;
                errors.push_back({{"range", range(document.source, found.span.offset, found.span.offset + found.span.length)},
                    {"severity", found.warning ? 2 : 1}, {"source", "dotk3"}, {"code", found.warning ? "warning" : "type"}, {"message", found.message}});
            }
        }
        catch (const dk3::Error &error) {
            errors.push_back({{"range", range(document.source, error.span.offset, error.span.offset + error.span.length)},
                {"severity", 1}, {"source", "dotk3"}, {"code", error.category}, {"message", error.what()}});
        }
        send({{"jsonrpc", "2.0"}, {"method", "textDocument/publishDiagnostics"},
            {"params", {{"uri", uri}, {"version", document.version}, {"diagnostics", errors}}}});
    }
    void update(const std::string &uri, int64_t version, const std::string &text) {
        if (uri.empty() || uri.size() > 8192) throw std::invalid_argument("Invalid document URI");
        auto previous = documents.find(uri);
        size_t oldBytes = previous == documents.end() ? 0 : previous->second.source->text.size();
        if (text.size() > maxDocument || textBytes - oldBytes + text.size() > maxText ||
            (previous == documents.end() && documents.size() >= maxDocuments)) throw std::invalid_argument("Document limit exceeded");
        textBytes = textBytes - oldBytes + text.size();
        Document document{version, std::make_shared<dk3::Source>(dk3::Source{uri, text}), {}, {}};
        auto inserted = documents.insert_or_assign(uri, std::move(document)); diagnostics(uri, inserted.first->second);
    }
public:
    bool exited = false;
    int exitCode = 1;
    void dispatch(const Json &message) {
        Json id = message.is_object() && message.contains("id") ? message["id"] : Json(nullptr);
        bool request = message.is_object() && message.contains("id");
        if (!message.is_object() || !message.contains("jsonrpc") || !message["jsonrpc"].is_string() || message["jsonrpc"] != "2.0" || !message.contains("method") || !message["method"].is_string() ||
            (request && !id.is_null() && !id.is_string() && !id.is_number_integer())) { failure(nullptr, -32600, "Invalid Request"); return; }
        std::string method = message["method"];
        Json params = message.value("params", Json::object());
        if (method == "exit") { exited = true; exitCode = shutdown ? 0 : 1; return; }
        if (method == "initialize") {
            if (!request || initialized || shutdown) { if (request) failure(id, -32600, "Already initialized"); return; }
            if (!params.is_object()) throw std::invalid_argument("Initialize params must be an object");
            initialized = true;
            result(id, {{"capabilities", {{"positionEncoding", "utf-16"}, {"textDocumentSync", {{"openClose", true}, {"change", 1}, {"save", true}}},
                {"documentSymbolProvider", true}, {"hoverProvider", true}, {"completionProvider", {{"triggerCharacters", Json::array({"."})}, {"resolveProvider", false}}}}}, {"serverInfo", {{"name", "dotk3-lsp"}, {"version", "alpha-63"}}}}); return;
        }
        if (!initialized) { if (request) failure(id, -32002, "Server not initialized"); return; }
        if (shutdown) { if (request) failure(id, -32600, "Server is shutting down"); return; }
        if (method == "shutdown") { if (request) { shutdown = true; result(id, nullptr); } return; }
        if (method == "initialized" || method == "$/cancelRequest" || method == "$/setTrace") return;
        if (method == "textDocument/didOpen") {
            const Json &document = params.at("textDocument");
            update(document.at("uri").get<std::string>(), versionNumber(document.at("version")), document.at("text").get<std::string>()); return;
        }
        if (method == "textDocument/didChange") {
            std::string uri = params.at("textDocument").at("uri").get<std::string>();
            auto found = documents.find(uri); if (found == documents.end()) throw std::invalid_argument("Document is not open");
            int64_t version = versionNumber(params.at("textDocument").at("version"));
            if (version <= found->second.version) return;
            const Json &changes = params.at("contentChanges");
            if (!changes.is_array() || changes.empty()) throw std::invalid_argument("Expected full document changes");
            for (const auto &change : changes) if (change.contains("range") || !change.at("text").is_string()) throw std::invalid_argument("Incremental edits are not supported");
            update(uri, version, changes.back().at("text").get<std::string>()); return;
        }
        if (method == "textDocument/didClose") {
            std::string uri = params.at("textDocument").at("uri").get<std::string>(); auto found = documents.find(uri);
            if (found != documents.end()) { textBytes -= found->second.source->text.size(); documents.erase(found); }
            send({{"jsonrpc", "2.0"}, {"method", "textDocument/publishDiagnostics"}, {"params", {{"uri", uri}, {"diagnostics", Json::array()}}}}); return;
        }
        if (method == "textDocument/didSave") {
            std::string uri = params.at("textDocument").at("uri").get<std::string>(); auto found = documents.find(uri);
            if (found != documents.end()) diagnostics(uri, found->second); return;
        }
        if (method == "textDocument/documentSymbol" && request) {
            std::string uri = params.at("textDocument").at("uri").get<std::string>(); auto found = documents.find(uri); Json symbols = Json::array();
            if (found != documents.end() && found->second.tree) for (const auto &node : found->second.tree->children)
                if (node->kind == "binding" || node->kind == "function" || node->kind == "record" || node->kind == "class" || node->kind == "trait" || node->kind == "impl" || node->kind == "enum") {
                    Json entry = symbol(node, found->second.tokens); if (!entry.is_null()) symbols.push_back(std::move(entry));
                }
            result(id, symbols); return;
        }
        if ((method == "textDocument/completion" || method == "textDocument/hover") && request) {
            std::string uri = params.at("textDocument").at("uri").get<std::string>(); auto found = documents.find(uri);
            if (found == documents.end()) { result(id, method == "textDocument/hover" ? Json(nullptr) : Json::array()); return; }
            size_t offset = bytePosition(found->second.source, params.at("position"));
            dk3::ModuleLoader loader = [&](const std::string &name) { return moduleSource(uri, name); };
            if (method == "textDocument/hover") {
                std::string detail = dk3::hoverText(found->second.source, offset, loader);
                result(id, detail.empty() ? Json(nullptr) : Json{{"contents", {{"kind", "markdown"}, {"value", "```dotk3\n" + detail + "\n```"}}}});
            } else {
                Json completions = Json::array();
                for (const auto &member : dk3::completeMembers(found->second.source, offset, loader)) {
                    Json item = {{"label", member.label}, {"kind", member.kind}, {"detail", member.detail}, {"insertText", member.label}};
                    if (!member.documentation.empty()) item["documentation"] = member.documentation;
                    completions.push_back(std::move(item));
                }
                result(id, completions);
            }
            return;
        }
        if (request) failure(id, -32601, "Method not found");
    }
};
}
int main() {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY); _setmode(_fileno(stdout), _O_BINARY);
#endif
    Server server;
    try {
        while (!server.exited) {
            auto body = readMessage(); if (!body) break;
            Json message;
            try { message = Json::parse(*body); }
            catch (const Json::parse_error &) { failure(nullptr, -32700, "Parse error"); continue; }
            try { server.dispatch(message); }
            catch (const Json::exception &error) {
                if (message.is_object() && message.contains("id")) failure(message["id"], -32602, error.what()); else log("Invalid notification params");
            } catch (const std::invalid_argument &error) {
                if (message.is_object() && message.contains("id")) failure(message["id"], -32602, error.what()); else log(error.what());
            }
        }
    } catch (const std::exception &error) { std::cerr << "dotk3-lsp: " << error.what() << '\n'; return 2; }
    if (!server.exited) {
        std::cerr << "dotk3-lsp: Input stream closed before the shutdown/exit handshake\n";
        return 1;
    }
    return server.exitCode;
}