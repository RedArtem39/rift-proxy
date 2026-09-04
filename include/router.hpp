#pragma once

#include <string>
#include <vector>
#include <memory>
#include <cstdint>

enum class RouteAction {
    DIRECT,
    PROXY,
    REJECT
};

enum class RuleType {
    DOMAIN_SUFFIX,
    DOMAIN_KEYWORD,
    DOMAIN_EXACT,
    IP_CIDR,
    FINAL_RULE
};

struct Rule {
    RuleType type = RuleType::FINAL_RULE;
    std::string pattern;
    RouteAction action = RouteAction::PROXY;

    // Parsed CIDR for IP_CIDR rules
    uint32_t ip_base = 0;
    uint32_t ip_mask = 0;
};

class Router {
public:
    Router() = default;
    
    void load_rules(const std::vector<std::string>& rule_strings, RouteAction default_action = RouteAction::PROXY);
    RouteAction match(const std::string& host, int port, std::string& matched_rule_info) const;
    const std::vector<Rule>& get_rules() const { return rules; }

private:
    std::vector<Rule> rules;
    RouteAction default_policy = RouteAction::PROXY;

    static bool parse_cidr(const std::string& cidr_str, uint32_t& ip_base, uint32_t& ip_mask);
    static bool ip_in_cidr(uint32_t ip, uint32_t ip_base, uint32_t ip_mask);
    static bool parse_ipv4(const std::string& ip_str, uint32_t& out_ip);
};
