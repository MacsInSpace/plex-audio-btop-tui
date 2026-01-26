#pragma once

#include <string>
#include <vector>
#include <map>

namespace PlexTUI {

// Simple XML parser for Plex API responses
class PlexXML {
public:
    struct Node {
        std::string name;
        std::map<std::string, std::string> attributes;
        std::string text;
        std::vector<Node> children;
        
        std::string get_attr(const std::string& key, const std::string& default_val = "") const {
            auto it = attributes.find(key);
            return it != attributes.end() ? it->second : default_val;
        }
        
        std::vector<Node> find_all(const std::string& name) const {
            std::vector<Node> results;
            for (const auto& child : children) {
                if (child.name == name) {
                    results.push_back(child);
                }
            }
            return results;
        }
        
        Node find_first(const std::string& name) const {
            for (const auto& child : children) {
                if (child.name == name) {
                    return child;
                }
            }
            return Node();
        }
    };
    
    static Node parse(const std::string& xml_string);
    
private:
    static Node parse_node(const std::string& xml, size_t& pos);
    static std::string parse_tag_name(const std::string& xml, size_t& pos);
    static std::map<std::string, std::string> parse_attributes(const std::string& xml, size_t& pos);
    static std::string unescape_xml(const std::string& str);
};

} // namespace PlexTUI
