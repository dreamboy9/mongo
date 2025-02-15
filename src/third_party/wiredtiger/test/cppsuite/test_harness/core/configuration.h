/*-
 * Public Domain 2014-present MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef CONFIGURATION_H
#define CONFIGURATION_H

#include <algorithm>
#include <string>
#include <stack>

extern "C" {
#include "test_util.h"
}

enum class types { BOOL, INT, STRING, STRUCT };

namespace test_harness {
class configuration {
    public:
    configuration(const std::string &test_config_name, const std::string &config)
    {
        const auto *config_entry = __wt_test_config_match(test_config_name.c_str());
        if (config_entry == nullptr)
            testutil_die(EINVAL, "failed to match test config name");
        std::string default_config = std::string(config_entry->base);
        /* Merge in the default configuration. */
        _config = merge_default_config(default_config, config);
        debug_print("Full config: " + _config, DEBUG_INFO);

        int ret = wiredtiger_test_config_validate(
          nullptr, nullptr, test_config_name.c_str(), _config.c_str());
        if (ret != 0)
            testutil_die(EINVAL, "failed to validate given config, ensure test config exists");
        ret =
          wiredtiger_config_parser_open(nullptr, _config.c_str(), _config.size(), &_config_parser);
        if (ret != 0)
            testutil_die(EINVAL, "failed to create configuration parser for provided config");
    }

    configuration(const WT_CONFIG_ITEM &nested)
    {
        if (nested.type != WT_CONFIG_ITEM::WT_CONFIG_ITEM_STRUCT)
            testutil_die(EINVAL, "provided config item isn't a structure");
        int ret = wiredtiger_config_parser_open(nullptr, nested.str, nested.len, &_config_parser);
        if (ret != 0)
            testutil_die(EINVAL, "failed to create configuration parser for provided sub config");
    }

    ~configuration()
    {
        if (_config_parser != nullptr) {
            _config_parser->close(_config_parser);
            _config_parser = nullptr;
        }
    }

    const std::string &
    get_config() const
    {
        return (_config);
    }

    /*
     * Wrapper functions for retrieving basic configuration values. Ideally tests can avoid using
     * the config item struct provided by wiredtiger.
     *
     * When getting a configuration value that may not exist for that configuration string or
     * component, the optional forms of the functions can be used. In this case a default value must
     * be passed and it will be set to that value.
     */
    std::string
    get_string(const std::string &key)
    {
        return get<std::string>(key, false, types::STRING, "", config_item_to_string);
    }

    std::string
    get_optional_string(const std::string &key, const std::string &def)
    {
        return get<std::string>(key, true, types::STRING, def, config_item_to_string);
    }

    bool
    get_bool(const std::string &key)
    {
        return get<bool>(key, false, types::BOOL, false, config_item_to_bool);
    }

    bool
    get_optional_bool(const std::string &key, const bool def)
    {
        return get<bool>(key, true, types::BOOL, def, config_item_to_bool);
    }

    int64_t
    get_int(const std::string &key)
    {
        return get<int64_t>(key, false, types::INT, 0, config_item_to_int);
    }

    int64_t
    get_optional_int(const std::string &key, const int64_t def)
    {
        return get<int64_t>(key, true, types::INT, def, config_item_to_int);
    }

    configuration *
    get_subconfig(const std::string &key)
    {
        return get<configuration *>(key, false, types::STRUCT, nullptr,
          [](WT_CONFIG_ITEM item) { return new configuration(item); });
    }

    configuration *
    get_optional_subconfig(const std::string &key)
    {
        return get<configuration *>(key, true, types::STRUCT, nullptr,
          [](WT_CONFIG_ITEM item) { return new configuration(item); });
    }

    private:
    static bool
    config_item_to_bool(const WT_CONFIG_ITEM item)
    {
        return (item.val != 0);
    }

    static int64_t
    config_item_to_int(const WT_CONFIG_ITEM item)
    {
        return (item.val);
    }

    static std::string
    config_item_to_string(const WT_CONFIG_ITEM item)
    {
        return std::string(item.str, item.len);
    }

    template <typename T>
    T
    get(const std::string &key, bool optional, types type, T def, T (*func)(WT_CONFIG_ITEM item))
    {
        WT_DECL_RET;
        WT_CONFIG_ITEM value = {"", 0, 1, WT_CONFIG_ITEM::WT_CONFIG_ITEM_BOOL};
        const char *error_msg = "Configuration value doesn't match requested type";

        ret = _config_parser->get(_config_parser, key.c_str(), &value);
        if (ret == WT_NOTFOUND && optional)
            return (def);
        else if (ret != 0)
            testutil_die(ret, "Error while finding config");

        if (type == types::STRING &&
          (value.type != WT_CONFIG_ITEM::WT_CONFIG_ITEM_STRING &&
            value.type != WT_CONFIG_ITEM::WT_CONFIG_ITEM_ID))
            testutil_die(-1, error_msg);
        else if (type == types::BOOL && value.type != WT_CONFIG_ITEM::WT_CONFIG_ITEM_BOOL)
            testutil_die(-1, error_msg);
        else if (type == types::INT && value.type != WT_CONFIG_ITEM::WT_CONFIG_ITEM_NUM)
            testutil_die(-1, error_msg);
        else if (type == types::STRUCT && value.type != WT_CONFIG_ITEM::WT_CONFIG_ITEM_STRUCT)
            testutil_die(-1, error_msg);

        return func(value);
    }

    /*
     * Merge together two configuration strings, the user one and the default one.
     */
    static std::string
    merge_default_config(const std::string &default_config, const std::string &user_config)
    {
        std::string merged_config;
        auto split_default_config = split_config(default_config);
        auto split_user_config = split_config(user_config);
        auto user_it = split_user_config.begin();
        for (auto default_it = split_default_config.begin();
             default_it != split_default_config.end(); ++default_it) {
            if (user_it->first != default_it->first)
                /* The default does not exist in the user configuration, add it. */
                merged_config += default_it->first + "=" + default_it->second;
            else {
                /* If we have a sub config merge it in. */
                if (user_it->second[0] == '(')
                    merged_config += default_it->first + "=(" +
                      merge_default_config(default_it->second, user_it->second) + ')';
                else
                    /* Add the user configuration as it exists. */
                    merged_config += user_it->first + "=" + user_it->second;
                ++user_it;
            }
            /* Add a comma after every item we add except the last one. */
            if (split_default_config.end() - default_it != 1)
                merged_config += ",";
        }
        return (merged_config);
    }

    /*
     * Split a config string into keys and values, taking care to not split incorrectly when we have
     * a sub config.
     */
    static std::vector<std::pair<std::string, std::string>>
    split_config(const std::string &config)
    {
        std::string cut_config = config;
        std::vector<std::pair<std::string, std::string>> split_config;
        std::string key = "", value = "";
        bool in_subconfig = false;
        bool expect_value = false;
        std::stack<char> subconfig_parens;

        /* All configuration strings must be at least 2 characters. */
        testutil_assert(config.size() > 1);

        /* Remove prefix and trailing "()". */
        if (config[0] == '(')
            cut_config = config.substr(1, config.size() - 2);

        size_t start = 0, len = 0;
        for (size_t i = 0; i < cut_config.size(); ++i) {
            if (cut_config[i] == '(') {
                subconfig_parens.push(cut_config[i]);
                in_subconfig = true;
            }
            if (cut_config[i] == ')') {
                subconfig_parens.pop();
                in_subconfig = !subconfig_parens.empty();
            }
            if (cut_config[i] == '=' && !in_subconfig) {
                expect_value = true;
                key = cut_config.substr(start, len);
                start += len + 1;
                len = 0;
                continue;
            }
            if (cut_config[i] == ',' && !in_subconfig) {
                expect_value = false;
                if (start + len >= cut_config.size())
                    break;
                value = cut_config.substr(start, len);
                start += len + 1;
                len = 0;
                split_config.push_back(std::make_pair(key, value));
                continue;
            }
            ++len;
        }
        if (expect_value) {
            value = cut_config.substr(start, len);
            split_config.push_back(std::make_pair(key, value));
        }

        /* We have to sort the config here otherwise we will match incorrectly while merging. */
        std::sort(split_config.begin(), split_config.end(), comparator);
        return (split_config);
    }

    static bool
    comparator(std::pair<std::string, std::string> a, std::pair<std::string, std::string> b)
    {
        return (a.first < b.first);
    }

    std::string _config;
    WT_CONFIG_PARSER *_config_parser = nullptr;
};
} // namespace test_harness

#endif
