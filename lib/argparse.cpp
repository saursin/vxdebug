#include "argparse.h"
#include "logger.h"
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

using namespace ArgParse;

////////////////////////////////////////////////////////////////////////////////
// Helpers

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
    const std::string& defaultval, bool required, const std::string& key) {
    
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
    if (arg.key == "") {        // convert the last alias as the key
        arg.key = alias2key(aliases[aliases.size()-1]);
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
    arg_list_.push_back(arg);
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

        size_t i = 0;
        while(i < args_.size()) {
            auto arg = args_[i];
            i++;

            // Check for positional argument
            if (arg[0] != '-') {
                parsed_pos_args_.push_back(arg);
                continue;
            }

            // Check for match
            bool found = false;
            Argument_t *argp=nullptr;
            
            for (auto &a: arg_list_) {
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

        if (argp->type == BOOL) {
            parsed_args_[argp->key].type = BOOL;
            parsed_args_[argp->key].value = true;
            continue;
        }
        else if (argp->type == INT) {
            if (i >= args_.size()) {
                throw ArgParseException("Missing value for argument: " + arg);
            }
            if (!is_valid_type(args_[i], INT)) {
                throw ArgParseException("Invalid value for argument " + arg + ": " + args_[i]);
            }
            parsed_args_[argp->key].type = INT;
            parsed_args_[argp->key].value = std::stoi(args_[i]);
            i++;
        }
        else if (argp->type == FLOAT) {
            if (i >= args_.size()) {
                throw ArgParseException("Missing value for argument: " + arg);
            }
            if (!is_valid_type(args_[i], FLOAT)) {
                throw ArgParseException("Invalid value for argument " + arg + ": " + args_[i]);
            }
            parsed_args_[argp->key].type = FLOAT;
            parsed_args_[argp->key].value = strtof(args_[i].c_str(), nullptr);
            i++;
        }
        else if (argp->type == STR) {
            if (i >= args_.size()) {
                throw ArgParseException("Missing value for argument: " + arg);
            }
            if (!is_valid_type(args_[i], STR)) {
                throw ArgParseException("Invalid value for argument " + arg + ": " + args_[i]);
            }
            parsed_args_[argp->key].type = STR;
            parsed_args_[argp->key].value = args_[i];
            i++;
        }
        else {
            throw ArgParseException("Unknown argument type: " + std::to_string(argp->type));
        }
        }

        // print help message if requested
        if (std::get<bool>(parsed_args_["help"].value)) {
            print_help();
            return 1;
        }

        // Check for required arguments
        for (const auto &a: arg_list_) {
            if (a.required && parsed_args_.find(a.key) == parsed_args_.end()) {
                throw ArgParseException("Required argument missing: " + a.key);
            }
        }

        return 0;
    }
    catch (const ArgParseException& e) {
        Logger::gerror("Argument parsing error: " + std::string(e.what()));
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
        if (a.type == BOOL) {
            printf("  ");
            for(size_t i=0; i<a.aliases.size(); i++) {
                printf("%s%s", a.aliases[i].c_str(), (i == a.aliases.size()-1) ? "  ": ", ");
            }
            printf("  %s\n", a.help.c_str());
        }
        else {
            printf("  %s: %s\n", a.key.c_str(), a.help.c_str());
            for (const auto& alias: a.aliases) {
                printf("    %s\n", alias.c_str());
            }
        }
    }

    if(epilog_.size() > 0)
        printf("%s\n", epilog_.c_str());

    printf("\n");
}
