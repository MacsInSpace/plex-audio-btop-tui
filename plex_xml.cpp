#include "plex_xml.h"
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>

namespace PlexTUI {

namespace {
void append_utf8(std::string& out, uint32_t cp) {
    if (cp <= 0x7F) {
        out += static_cast<char>(cp);
    } else if (cp <= 0x7FF) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp <= 0xFFFF) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp <= 0x10FFFF) {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}
}  // namespace

PlexXML::Node PlexXML::parse(const std::string& xml_string) {
    Node empty_node;
    if (xml_string.empty()) return empty_node;
    
    size_t pos = 0;
    // Skip XML declaration if present
    if (xml_string.find("<?xml") == 0) {
        pos = xml_string.find("?>");
        if (pos != std::string::npos) {
            pos += 2;
            // Skip whitespace
            while (pos < xml_string.length() && std::isspace(xml_string[pos])) pos++;
        } else {
            pos = 0;  // Malformed XML declaration, start from beginning
        }
    }
    
    if (pos >= xml_string.length()) return empty_node;
    
    try {
        return parse_node(xml_string, pos);
    } catch (...) {
        return empty_node;  // Return empty node on parse error
    }
}

PlexXML::Node PlexXML::parse_node(const std::string& xml, size_t& pos) {
    Node node;
    
    if (pos >= xml.length()) return node;
    
    // Skip whitespace
    while (pos < xml.length() && std::isspace(xml[pos])) pos++;
    if (pos >= xml.length() || xml[pos] != '<') {
        return node;
    }
    
    pos++; // Skip '<'
    if (pos >= xml.length()) return node;
    
    // Check for closing tag
    if (xml[pos] == '/') {
        return node; // Closing tag, return empty
    }
    
    // Parse tag name
    node.name = parse_tag_name(xml, pos);
    
    // Parse attributes
    node.attributes = parse_attributes(xml, pos);
    
    // Check for self-closing tag
    if (pos < xml.length() && xml[pos] == '/') {
        pos += 2; // Skip '/>'
        return node;
    }
    
    if (pos >= xml.length() || xml[pos] != '>') {
        return node;
    }
    pos++; // Skip '>'
    
    // Parse content
    size_t content_start = pos;
    while (pos < xml.length()) {
        if (xml[pos] == '<') {
            if (pos + 1 < xml.length() && xml[pos + 1] == '/') {
                // Closing tag
                std::string text = xml.substr(content_start, pos - content_start);
                node.text = unescape_xml(text);
                // Skip closing tag
                pos = xml.find('>', pos);
                if (pos != std::string::npos) pos++;
                break;
            } else {
                // Child node
                if (pos > content_start) {
                    std::string text = xml.substr(content_start, pos - content_start);
                    node.text += unescape_xml(text);
                }
                Node child = parse_node(xml, pos);
                if (!child.name.empty()) {
                    node.children.push_back(child);
                }
                content_start = pos;
            }
        } else {
            pos++;
        }
    }
    
    return node;
}

std::string PlexXML::parse_tag_name(const std::string& xml, size_t& pos) {
    size_t start = pos;
    while (pos < xml.length() && !std::isspace(xml[pos]) && 
           xml[pos] != '>' && xml[pos] != '/') {
        pos++;
    }
    return xml.substr(start, pos - start);
}

std::map<std::string, std::string> PlexXML::parse_attributes(const std::string& xml, size_t& pos) {
    std::map<std::string, std::string> attrs;
    
    while (pos < xml.length() && xml[pos] != '>' && xml[pos] != '/') {
        // Skip whitespace
        while (pos < xml.length() && std::isspace(xml[pos])) pos++;
        if (pos >= xml.length() || xml[pos] == '>' || xml[pos] == '/') break;
        
        // Parse key
        size_t key_start = pos;
        while (pos < xml.length() && xml[pos] != '=' && !std::isspace(xml[pos])) pos++;
        std::string key = xml.substr(key_start, pos - key_start);
        
        // Skip to '='
        while (pos < xml.length() && xml[pos] != '=') pos++;
        if (pos >= xml.length()) break;
        pos++; // Skip '='
        
        // Skip whitespace
        while (pos < xml.length() && std::isspace(xml[pos])) pos++;
        
        // Parse value (quoted)
        char quote = xml[pos];
        if (quote != '"' && quote != '\'') break;
        pos++; // Skip quote
        
        size_t value_start = pos;
        while (pos < xml.length() && xml[pos] != quote) pos++;
        std::string value = xml.substr(value_start, pos - value_start);
        pos++; // Skip closing quote
        
        attrs[key] = unescape_xml(value);
    }
    
    return attrs;
}

std::string PlexXML::unescape_xml(const std::string& str) {
    std::string result;
    result.reserve(str.size());
    for (size_t i = 0; i < str.size(); ++i) {
        if (str[i] != '&') {
            result += str[i];
            continue;
        }
        if (i + 4 <= str.size() && str.compare(i, 4, "&lt;") == 0) {
            result += '<';
            i += 3;
            continue;
        }
        if (i + 4 <= str.size() && str.compare(i, 4, "&gt;") == 0) {
            result += '>';
            i += 3;
            continue;
        }
        if (i + 5 <= str.size() && str.compare(i, 5, "&amp;") == 0) {
            result += '&';
            i += 4;
            continue;
        }
        if (i + 6 <= str.size() && str.compare(i, 6, "&quot;") == 0) {
            result += '"';
            i += 5;
            continue;
        }
        if (i + 6 <= str.size() && str.compare(i, 6, "&apos;") == 0) {
            result += '\'';
            i += 5;
            continue;
        }
        if (i + 2 < str.size() && str[i + 1] == '#') {
            size_t start = i + 2;
            int base = 10;
            if (start < str.size() && (str[start] == 'x' || str[start] == 'X')) {
                base = 16;
                start++;
            }
            size_t end = start;
            while (end < str.size()) {
                char c = str[end];
                if (base == 10 && c >= '0' && c <= '9') { end++; continue; }
                if (base == 16 && ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) { end++; continue; }
                break;
            }
            if (end > start && end < str.size() && str[end] == ';') {
                std::string num = str.substr(start, end - start);
                unsigned long val = std::strtoul(num.c_str(), nullptr, base);
                if (val <= 0x10FFFF) {
                    append_utf8(result, static_cast<uint32_t>(val));
                    i = end;
                    continue;
                }
            }
        }
        result += str[i];
    }
    return result;
}

} // namespace PlexTUI
