#include "router.hpp"
#include "logger.hpp"
#include <sstream>
#include <algorithm>
#include <ws2tcpip.h>

static std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return (char)std::tolower(c); });
    return s;
}

static RouteAction string_to_action(const std::string& act) {
    std::string upper = act;
    std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c){ return (char)::toupper(c); });
    if (upper == "DIRECT") return RouteAction::DIRECT;
    if (upper == "REJECT" || upper == "BLOCK") return RouteAction::REJECT;
    return RouteAction::PROXY;
}

bool Router::parse_ipv4(const std::string& ip_str, uint32_t& out_ip) {
    in_addr addr;
    if (inet_pton(AF_INET, ip_str.c_str(), &addr) == 1) {
        out_ip = ntohl(addr.s_addr);
        return true;
    }
    return false;
}

bool Router::parse_cidr(const std::string& cidr_str, uint32_t& ip_base, uint32_t& ip_mask) {
    size_t slash = cidr_str.find('/');
    if (slash == std::string::npos) {
        if (!parse_ipv4(cidr_str, ip_base)) return false;
        ip_mask = 0xFFFFFFFF;
        return true;
    }

    std::string ip_part = cidr_str.substr(0, slash);
    std::string prefix_part = cidr_str.substr(slash + 1);

    if (!parse_ipv4(ip_part, ip_base)) return false;

    int prefix = std::stoi(prefix_part);
    if (prefix < 0 || prefix > 32) return false;

    ip_mask = (prefix == 0) ? 0 : (0xFFFFFFFF << (32 - prefix));
    ip_base &= ip_mask;
    return true;
}

bool Router::ip_in_cidr(uint32_t ip, uint32_t ip_base, uint32_t ip_mask) {
    return (ip & ip_mask) == ip_base;
}

void Router::load_rules(const std::vector<std::string>& rule_strings, RouteAction default_action) {
    rules.clear();
    default_policy = default_action;

    for (const auto& line : rule_strings) {
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;

        std::vector<std::string> tokens;
        std::istringstream ss(line);
        std::string token;
        while (std::getline(ss, token, ',')) {
            // Trim whitespace
            size_t first = token.find_first_not_of(" \t\r\n");
            size_t last = token.find_last_not_of(" \t\r\n");
            if (first != std::string::npos && last != std::string::npos) {
                tokens.push_back(token.substr(first, last - first + 1));
            }
        }

        if (tokens.size() < 2) continue;

        std::string type_str = tokens[0];
        std::transform(type_str.begin(), type_str.end(), type_str.begin(), [](unsigned char c){ return (char)::toupper(c); });

        Rule rule;
        if (type_str == "DOMAIN-SUFFIX" && tokens.size() >= 3) {
            rule.type = RuleType::DOMAIN_SUFFIX;
            rule.pattern = to_lower(tokens[1]);
            rule.action = string_to_action(tokens[2]);
            rules.push_back(rule);
        } else if (type_str == "DOMAIN-KEYWORD" && tokens.size() >= 3) {
            rule.type = RuleType::DOMAIN_KEYWORD;
            rule.pattern = to_lower(tokens[1]);
            rule.action = string_to_action(tokens[2]);
            rules.push_back(rule);
        } else if (type_str == "DOMAIN" && tokens.size() >= 3) {
            rule.type = RuleType::DOMAIN_EXACT;
            rule.pattern = to_lower(tokens[1]);
            rule.action = string_to_action(tokens[2]);
            rules.push_back(rule);
        } else if (type_str == "IP-CIDR" && tokens.size() >= 3) {
            rule.type = RuleType::IP_CIDR;
            rule.pattern = tokens[1];
            rule.action = string_to_action(tokens[2]);
            if (parse_cidr(tokens[1], rule.ip_base, rule.ip_mask)) {
                rules.push_back(rule);
            }
        } else if (type_str == "FINAL" || type_str == "MATCH") {
            rule.type = RuleType::FINAL_RULE;
            rule.pattern = "MATCH";
            rule.action = string_to_action(tokens[1]);
            rules.push_back(rule);
        }
    }
}

RouteAction Router::match(const std::string& host, int port, std::string& matched_rule_info) const {
    (void)port;
    std::string lower_host = to_lower(host);
    uint32_t ip_val = 0;
    bool is_ip = parse_ipv4(host, ip_val);

    for (const auto& r : rules) {
        if (r.type == RuleType::DOMAIN_SUFFIX) {
            if (lower_host == r.pattern || 
                (lower_host.size() > r.pattern.size() && 
                 lower_host.rfind("." + r.pattern) == lower_host.size() - r.pattern.size() - 1)) {
                matched_rule_info = "DOMAIN-SUFFIX," + r.pattern;
                return r.action;
            }
        } else if (r.type == RuleType::DOMAIN_KEYWORD) {
            if (lower_host.find(r.pattern) != std::string::npos) {
                matched_rule_info = "DOMAIN-KEYWORD," + r.pattern;
                return r.action;
            }
        } else if (r.type == RuleType::DOMAIN_EXACT) {
            if (lower_host == r.pattern) {
                matched_rule_info = "DOMAIN," + r.pattern;
                return r.action;
            }
        } else if (r.type == RuleType::IP_CIDR) {
            if (is_ip && ip_in_cidr(ip_val, r.ip_base, r.ip_mask)) {
                matched_rule_info = "IP-CIDR," + r.pattern;
                return r.action;
            }
        } else if (r.type == RuleType::FINAL_RULE) {
            matched_rule_info = "FINAL";
            return r.action;
        }
    }

    matched_rule_info = "DEFAULT_POLICY";
    return default_policy;
}
