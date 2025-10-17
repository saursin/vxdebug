#include "argparse.h"
#include <iostream>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <algorithm>
#include <set>

using namespace ArgParse;

////////////////////////////////////////////////////////////////////////////////
// Helpers

// Check if a string is a negative number (starts with - followed by digits)
bool is_negative_number(const std::string& str) {
    if (str.length() < 2 || str[0] != '-') {
        return false;
    }
    
    // Check if the rest is a valid number
    for (size_t i = 1; i < str.length(); i++) {
        if (!std::isdigit(str[i]) && str[i] != '.') {
            return false;
        }
    }
    
    return true;
}

// Check if value is in allowed choices
bool is_valid_choice(const std::string& value, const std::vector<std::string>& choices) {
    if (choices.empty()) {
        return true;  // No choices restriction
    }
    
    return std::find(choices.begin(), choices.end(), value) != choices.end();
}

// Convert alias to key
// '--opt-flat' -> 'opt_flat'
std::string ArgParse::alias2key(const std::string& alias) {
    bool arg_started = false;
    std::string key = "";
    for(size_t i=0; i<alias.size(); i++) {
        if (arg_started) {
            if((alias[i] >= 'A' && alias[i] <= 'Z') || 
                (alias[i] >= 'a' && alias[i] <= 'z') || 
                (alias[i] >= '0' && alias[i] <= '9') ||
                (alias[i] == '_')) {
                key += alias[i];
            }
            else if(alias[i] == '-') {
                key += "_";
            }
            else {
                throw ArgParseException("Invalid alias: " + alias);
            }
        }
        else {
            if(alias[i] == '-') {
                // Skip prefix dashes
            }
            else if((alias[i] >= 'A' && alias[i] <= 'Z') || 
                    (alias[i] >= 'a' && alias[i] <= 'z')) {
                arg_started = true;
                key += alias[i];
            }
            else {
                throw ArgParseException("Invalid alias: " + alias);
            }
        }
    }
    return key;
}

bool ArgParse::is_valid_type(const std::string& str, ArgType_t type) {
    if (type == BOOL) {
        return (str == "true" || str == "1" || str == "false" || str == "0");
    }
    else if (type == INT) {
        if (str.empty()) return false;
        size_t start = 0;
        // Handle negative numbers
        if (str[0] == '-') {
            if (str.size() == 1) return false;
            start = 1;
        }
        for (size_t i = start; i < str.size(); i++) {
            if (str[i] < '0' || str[i] > '9') {
                return false;
            }
        }
        return true;
    }
    else if (type == FLOAT) {
        if (str.empty()) return false;
        bool decimal_point_found = false;
        size_t start = 0;
        // Handle negative numbers
        if (str[0] == '-') {
            if (str.size() == 1) return false;
            start = 1;
        }
        for (size_t i = start; i < str.size(); i++) {
            if (str[i] < '0' || str[i] > '9') {
                if (str[i] == '.' && !decimal_point_found && i != str.size()-1) {
                    decimal_point_found = true;
                    continue;
                }
                else {
                    return false;
                }
            }
        }
        return true;
    }
    else if (type == STR) {
        return true;
    }
    else {
        throw ArgParseException("Unknown argument type:" + std::to_string(type));
    }
}


////////////////////////////////////////////////////////////////////////////////
// ArgumentParser Class

ArgumentParser::ArgumentParser(const std::string& prog_name, const std::string& description, const std::string& epilog):
    prog_name_(prog_name),
    description_(description),
    epilog_(epilog) 
{
    // Add help argument
    add_argument({"-h", "--help"}, "Show this help message and exit");
}


void ArgumentParser::add_argument(const std::vector<std::string>& aliases, const std::string& help, ArgType_t type, 
    const std::string& defaultval, bool required, const std::string& key, const std::vector<std::string>& choices, const std::string& metavar) {
    
    if(aliases.size() == 0) {
        throw ArgParseException("No aliases provided");
    }

    if (required && defaultval != "") {
        throw ArgParseException("Argument cannot be both required and have a default value: " + key);
    }

    Argument_t arg;
    arg.aliases = aliases;
    arg.help = help;
    arg.type = type;
    arg.required = required;
    arg.defaultval.type = UNK;  // Initialize to unknown type
    arg.key = key;
    
    // Detect if this is a positional argument (Python-style)
    // Positional arguments have aliases that don't start with '-'
    arg.is_positional = true;
    for (const auto& alias : aliases) {
        if (alias.length() > 0 && alias[0] == '-') {
            arg.is_positional = false;
            break;
        }
    }
    
    if (arg.key == "") {        // convert an alias as the key
        if (arg.is_positional) {
            // For positional arguments, use the alias directly as the key
            arg.key = aliases[0];
        } else {
            // For optional arguments, prefer the longest alias (usually the --long form)
            std::string best_alias = aliases[0];
            for (const auto& alias : aliases) {
                if (alias.length() > best_alias.length()) {
                    best_alias = alias;
                }
            }
            arg.key = alias2key(best_alias);
        }
    }
    

    if (defaultval != "") {
        if (type == BOOL) {
            arg.defaultval.type = BOOL;
            arg.defaultval.value = (defaultval == "true" || defaultval == "1") ? true : false;
        }
        else if (type == INT) {
            arg.defaultval.type = INT;
            arg.defaultval.value = std::stoi(defaultval);
        }
        else if (type == FLOAT) {
            arg.defaultval.type = FLOAT;
            arg.defaultval.value = strtof(defaultval.c_str(), nullptr);
        }
        else if (type == STR) {
            arg.defaultval.type = STR;
            arg.defaultval.value = defaultval;
        }
        else {
            throw ArgParseException("Unknown argument type: " + std::to_string(type));
        }  
    }
    else {
        arg.defaultval.type = UNK;
    }
    
    // Set choices for validation
    arg.choices = choices;
    
    // Set metavar
    arg.metavar = metavar;
    
    // Add to appropriate list
    arg_list_.push_back(arg);
    if (arg.is_positional) {
        pos_arg_list_.push_back(arg);
    }
}


int ArgumentParser::parse_args(int argc, char **argv) {
    std::vector<std::string> args;
    for (int i = 0; i < argc; i++) {
        args.push_back(std::string(argv[i]));
    }
    return parse_args(args);
}

int ArgumentParser::parse_args(const std::vector<std::string>& args) {
    // TODO: Need to check alias collisions

    args_.clear();
    args_ = args;   // copy input args

    parsed_args_.clear();
    parsed_pos_args_.clear();

    try {
        // Take program name from args if not set
        if (prog_name_.size() == 0) {
            prog_name_ = args_[0];
        }

        // Track which arguments were provided (not just initialized)
        std::set<std::string> provided_args;
        
        // add bool args and set default values
        for (auto &a: arg_list_) {
            if (a.type == BOOL) {
                // All BOOL args get added with false as default
                parsed_args_[a.key].type = BOOL;
                parsed_args_[a.key].value = false;
            }
            else if(a.defaultval.type != UNK) {
                // Non-BOOL args with explicit defaults
                parsed_args_[a.key].type = a.type;
                parsed_args_[a.key] = a.defaultval;
                provided_args.insert(a.key);  // Defaults count as provided
            }
            else {
                // Non-BOOL args without defaults - add with appropriate empty values
                parsed_args_[a.key].type = a.type;
                switch(a.type) {
                    case INT:
                        parsed_args_[a.key].value = 0;
                        break;
                    case FLOAT:
                        parsed_args_[a.key].value = 0.0f;
                        break;
                    case STR:
                        parsed_args_[a.key].value = std::string("");
                        break;
                    default:
                        break;
                }
            }
        }

        // Parse args
        if (!args_.empty())
            args_.erase(args_.begin()); // remove program name

        // Check for help first, before any parsing
        for (const auto& arg : args_) {
            if (arg == "-h" || arg == "--help") {
                parsed_args_["help"].type = BOOL;
                parsed_args_["help"].value = true;
                print_help();
                return 1;
            }
        }

        // Sequential parsing like Python's argparse
        std::vector<std::string> positional_values;
        
        size_t i = 0;
        while(i < args_.size()) {
            auto arg = args_[i];
            i++;

            // Check if this is an optional argument (starts with - but not a negative number)
            if (arg[0] == '-' && !is_negative_number(arg)) {
                // Find matching optional argument
                bool found = false;
                Argument_t *argp = nullptr;
                
                for (auto &a: arg_list_) {
                    if (a.is_positional) continue;  // Skip positional arguments
                    for (auto alias: a.aliases) {
                        if (alias == arg) {
                            found = true;
                            argp = &a;
                            break;
                        }
                    }
                    if (found) break;
                }

                if (!found) {
                    throw ArgParseException("Unknown argument: " + arg);
                }

                // Handle optional argument
                if (argp->type == BOOL) {
                    parsed_args_[argp->key].type = BOOL;
                    parsed_args_[argp->key].value = true;
                    provided_args.insert(argp->key);
                }
                else {
                    // Need next argument as value
                    if (i >= args_.size()) {
                        throw ArgParseException("Missing value for argument: " + arg);
                    }
                    const std::string& value = args_[i];
                    i++;
                    
                    parsed_args_[argp->key].type = argp->type;
                    switch(argp->type) {
                        case INT:
                            if (!is_valid_type(value, INT)) {
                                throw ArgParseException("Invalid integer value for " + arg + ": " + value);
                            }
                            parsed_args_[argp->key].value = std::stoi(value);
                            break;
                        case FLOAT:
                            if (!is_valid_type(value, FLOAT)) {
                                throw ArgParseException("Invalid float value for " + arg + ": " + value);
                            }
                            parsed_args_[argp->key].value = strtof(value.c_str(), nullptr);
                            break;
                        case STR:
                            parsed_args_[argp->key].value = value;
                            break;
                        default:
                            throw ArgParseException("Unknown argument type for " + arg);
                    }
                    
                    // Mark as provided
                    provided_args.insert(argp->key);
                    
                    // Validate choices
                    if (!is_valid_choice(value, argp->choices)) {
                        std::string choices_str = "";
                        for (size_t j = 0; j < argp->choices.size(); j++) {
                            if (j > 0) choices_str += ", ";
                            choices_str += "'" + argp->choices[j] + "'";
                        }
                        throw ArgParseException("Invalid choice for " + arg + ": '" + value + "' (choose from " + choices_str + ")");
                    }
                }
            } else {
                // This is a positional argument
                positional_values.push_back(arg);
                parsed_pos_args_.push_back(arg);  // For backward compatibility
            }
        }
        
        // Assign positional arguments to their defined parameters
        for (size_t pos_idx = 0; pos_idx < pos_arg_list_.size(); pos_idx++) {
            const auto& pos_arg = pos_arg_list_[pos_idx];
            if (pos_idx < positional_values.size()) {
                // Assign the positional value
                const std::string& value = positional_values[pos_idx];
                parsed_args_[pos_arg.key].type = pos_arg.type;
                
                switch(pos_arg.type) {
                    case INT:
                        if (!is_valid_type(value, INT)) {
                            throw ArgParseException("Invalid integer value for " + pos_arg.key + ": " + value);
                        }
                        parsed_args_[pos_arg.key].value = std::stoi(value);
                        break;
                    case FLOAT:
                        if (!is_valid_type(value, FLOAT)) {
                            throw ArgParseException("Invalid float value for " + pos_arg.key + ": " + value);
                        }
                        parsed_args_[pos_arg.key].value = strtof(value.c_str(), nullptr);
                        break;
                    case STR:
                        parsed_args_[pos_arg.key].value = value;
                        break;
                    case BOOL:
                        if (!is_valid_type(value, BOOL)) {
                            throw ArgParseException("Invalid boolean value for " + pos_arg.key + ": " + value);
                        }
                        parsed_args_[pos_arg.key].value = (value == "true" || value == "1");
                        break;
                    default:
                        throw ArgParseException("Unknown argument type for " + pos_arg.key);
                }
                
                // Mark positional argument as provided
                provided_args.insert(pos_arg.key);
                
                // Validate choices for positional arguments
                if (!is_valid_choice(value, pos_arg.choices)) {
                    std::string choices_str = "";
                    for (size_t j = 0; j < pos_arg.choices.size(); j++) {
                        if (j > 0) choices_str += ", ";
                        choices_str += "'" + pos_arg.choices[j] + "'";
                    }
                    throw ArgParseException("Invalid choice for " + pos_arg.key + ": '" + value + "' (choose from " + choices_str + ")");
                }
            } else if (pos_arg.required) {
                throw ArgParseException("Missing required positional argument: " + pos_arg.key);
            }
        }

        // Help is handled earlier in parsing

        // Check for required arguments
        for (const auto &a: arg_list_) {
            if (a.required && provided_args.find(a.key) == provided_args.end()) {
                throw ArgParseException("Required argument missing: " + a.key);
            }
        }

        return 0;
    }
    catch (const ArgParseException& e) {
        std::cerr << "Argument parsing error: " << e.what() << std::endl;
        return -1;
    }
}

void ArgumentParser::print_args() const {
    printf("Args:\n");
    for (const auto &k: parsed_args_){
        printf("  %s: ", k.first.c_str());
        switch (k.second.type)
        {
            case BOOL:  printf("<bool> %s\n", std::get<bool>(k.second.value) ? "true": "false"); break;
            case INT:   printf("<int> %d\n", std::get<int>(k.second.value)); break;
            case FLOAT: printf("<float> %f\n", std::get<float>(k.second.value)); break;
            case STR:   printf("<str> %s\n", std::get<std::string>(k.second.value).c_str()); break;
        default:
            printf("<unk> ??\n"); break;
        }
    }

    printf("\nPositional Args: [");
    for (const auto& k: parsed_pos_args_){
        printf("'%s' ", k.c_str());
    }
    printf("]\n");
}


void ArgumentParser::print_help() const {
    printf("Usage: %s [options] [args]\n", prog_name_.c_str());

    if(description_.size() > 0)
        printf("Description: %s\n", description_.c_str());

    printf("\nOptions:\n");
    for (const auto& a: arg_list_){
        if (a.is_positional) continue;  // Skip positional args in options section
        
        printf("  ");
        for(size_t i=0; i<a.aliases.size(); i++) {
            printf("%s", a.aliases[i].c_str());
            if (a.type != BOOL) {
                // Show metavar or generate default
                std::string meta = a.metavar;
                if (meta.empty()) {
                    switch(a.type) {
                        case INT: meta = "N"; break;
                        case FLOAT: meta = "F"; break;
                        case STR: meta = "STR"; break;
                        default: meta = "VALUE"; break;
                    }
                }
                printf(" %s", meta.c_str());
            }
            if (i < a.aliases.size() - 1) printf(", ");
        }
        printf("\n    %s\n", a.help.c_str());
        
        // Show choices if available
        if (!a.choices.empty()) {
            printf("    choices: {");
            for (size_t i = 0; i < a.choices.size(); i++) {
                printf("'%s'%s", a.choices[i].c_str(), (i < a.choices.size() - 1) ? ", " : "");
            }
            printf("}\n");
        }
    }
    
    // Show positional arguments
    bool has_positional = false;
    for (const auto& a: arg_list_) {
        if (a.is_positional) {
            if (!has_positional) {
                printf("\nPositional arguments:\n");
                has_positional = true;
            }
            std::string meta = a.metavar.empty() ? a.key : a.metavar;
            printf("  %s\n    %s\n", meta.c_str(), a.help.c_str());
            
            // Show choices if available
            if (!a.choices.empty()) {
                printf("    choices: {");
                for (size_t i = 0; i < a.choices.size(); i++) {
                    printf("'%s'%s", a.choices[i].c_str(), (i < a.choices.size() - 1) ? ", " : "");
                }
                printf("}\n");
            }
        }
    }

    if(epilog_.size() > 0)
        printf("%s\n", epilog_.c_str());

    printf("\n");
}
