/*
Copyright 2018 Google LLC

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    https://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

/**
 * G A L O G E N
 *
 * Galogen generates headers, as well as code to load OpenGL entry points 
 * for the exact API version, profile and extensions that you specify.
 *
 */

#include "third_party/tinyxml2.h"
#include <algorithm>
#include <functional>
#include <memory>
#include <regex>
#include <sstream>
#include <stdio.h>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace galogen {

// Information about an API type, such as GLuint or GLfloat.
struct TypeInfo {  
  // Name of this type.
  std::string name;
  
  // Legal C code for type declaration.
  std::string type_cdecl;
  
  // Name of another type that this type requires.
  std::string requires;
  
  // API name for which this type definition applies.
  std::string api;
};

// Information about an enumerant, i.e. GL_TEXTURE_2D.
struct EnumerantInfo {  
  // Name of this enumerant.
  std::string name;
  
  // Alternative name for this enumerant.
  std::string alias;
  
  // Value of this enumerant.
  std::string value;
  
  // Legal C suffix to append to the value;
  std::string suffix;
  
  // API name for which this enumerant definition applies.
  std::string api;
};

// Information about enumerant group.
struct GroupInfo {
   // Name of the group, i.e. "AccumOp".
   std::string name;
   
   // List of enumerants that are members of this group.
   std::vector<const EnumerantInfo*> enums;
   
   // Always empty.
   std::string api;
};


// Information about an API command, i.e. glBindTexture
struct CommandInfo {
  // Information about a command parameter.
  struct ParamInfo {    
    // Name of the parameter.
    std::string name;
    
    // The full C type of the parameter (e.g. "const GLfloat*").
    std::string ctype;
    
    // Name of the API type that this parameter's type references.
    // For example, if ctype is "const GLfloat*", this will be "GLfloat".
    // If ctype doesn't reference any API-specific types (e.g. "const void*"),
    // this field will be empty.
    std::string referenced_api_type;
    
    // Group of this parameter.
    std::string group;
    
    // According to official docs, "parameter length, either an integer
    // specifying the number of elements of the parameter r a complex string
    // expression with poorly defined syntax, usually representing a length that
    // is computed as a combination of other command parameter values, and
    // possibly current GL state as well."
    // Take from that what you will.
    std::string len;
  };
   
  // Name of this command.
  std::string name;
  
  // Corresponding C function prototype, up to and including the function name,
  // but not the parameters.
  std::string prototype;
  
  // C type returned by this command, e.g. "const GLchar*".
  std::string return_ctype;
  
  // API-specific type, such as GLuint, referenced by this command's return
  // type. If the command's return type doesn't reference any API-specific types
  // (e.g. "void"), this field is empty.
  std::string referenced_api_type;
  
  // List of this command's parameters.
  std::vector<ParamInfo> parameters;
 
  // Name of another command that this command is an alias of.
  std::string alias;
  
  // Name of another command that is the vector equivalent for
  // this command.
  std::string vecequiv;
   
  // Always empty.
  std::string api;
};

// If you want to write a custom output generator for Galogen, you must
// implement the following interface.
class OutputGenerator {
public:
  virtual ~OutputGenerator() = default;

  // Invoked at the very start of output generation. This is where you should
  // do any setup, such as opening output files.
  virtual void start(const std::string &name,
                     const std::string &api_name,
                     const std::string &profile,
                     int api_ver_maj,
                     int api_ver_min){}
  
  virtual void processType(const TypeInfo &type){}
  virtual void processEnumGroup(const GroupInfo &group){}
  virtual void processEnumerant(const EnumerantInfo &type){}
  virtual void processCommand(const CommandInfo &type){}
  
  // Invoked at the end of output generation.
  virtual void end(){}
};

#define FAIL(...) {\
  fprintf(stderr, "FATAL ERROR: " __VA_ARGS__ ); \
  exit(1); \
}

#define FAIL_IF(cond, ...) { \
  if ((cond)) { FAIL(__VA_ARGS__); } \
}

// ----------------------------------------------------------------------------

namespace internal {

template <class A, class B>
using XmlIter =
typename std::conditional<
    std::is_const<typename std::remove_pointer<A>::type>::value,
                  const B*, B*>::type;

#define FOR_EACH_CHILD(n, as) \
  for (XmlIter<decltype(n), tinyxml2::XMLNode> as = n->FirstChild(); \
       as != nullptr; as = as->NextSibling())

#define FOR_EACH_CHILD_ELEM_NAMED(name, n, as) \
  for(XmlIter<decltype(n), tinyxml2::XMLElement> as = \
          n->FirstChildElement(name); \
      as != nullptr; as = as->NextSiblingElement(name))

#define FOR_EACH_CHILD_ELEM(n, as) FOR_EACH_CHILD_ELEM_NAMED(nullptr, n, as)

// An API entity is a type, enum or command.
// An entity is defined by data extracted from its corresponding XML element.
// However, the same entity may be represented by differently in
// different APIs (e.g. an enum might have different values in GL vs GL ES).
template <class T>
class ApiEntity {
public:
  void add(const T &e) { set_.push_back(e); }
  
  const T* get(const char *api) const {
    const T *result = nullptr;
    for (const T &e : set_) {
      if ((e.api.empty()  && result == nullptr) ||
          (!e.api.empty() && e.api == api)) {
        result = &e;
      } 
    }
    return result;
  }
  
  // Mark this entity as processed.
  void markProcessed() { processed_ = true; }

  // Return true if the entity is marked as processed.
  bool isProcessed() const { return processed_; }

private:
  std::vector<T> set_;
  bool processed_ = false;
};

// Maps names to API entities.
template <class T>
using EntityMap = std::unordered_map<std::string, ApiEntity<T>>;

void populateEntity(TypeInfo *info, const tinyxml2::XMLElement *e) {
  if (const char *name_attrib = e->Attribute("name")) {
    info->name = name_attrib;
  }
  if (const char *requires_attrib = e->Attribute("requires")) {
    info->requires = requires_attrib;
  }
  if (const char *api_attrib = e->Attribute("api")) {
    info->api = api_attrib;
  }
  FOR_EACH_CHILD(e, child) {
    if (const tinyxml2::XMLText *text = child->ToText()) {
      info->type_cdecl += text->Value();
    } else if (const tinyxml2::XMLElement *elem = child->ToElement()) {
      const char *tag_name = elem->Value();
      if (strcmp(tag_name, "name") == 0) {
        info->name = elem->GetText();
        info->type_cdecl += " " + info->name;
      } else if (strcmp(tag_name, "apientry") == 0) {
        info->type_cdecl += " GL_APIENTRY ";
      } else {
        FAIL("Unexpected element \"%s\" in type definition on line %d\n",
              tag_name,
              e->GetLineNum());
      }
    }
  }
  FAIL_IF(info->name.empty(), "Type missing \"name\" attribute on line %d\n",
          e->GetLineNum());
}

void populateEntity(EnumerantInfo *info, const tinyxml2::XMLElement *e) {
  info->name = e->Attribute("name");
  info->value = e->Attribute("value");
  FAIL_IF(info->name.empty() || info->value.empty(),
          "Enumerant missing \"name\" or \"value\" attribute on line %d\n",
          e->GetLineNum());
  if (const char *type_attrib = e->Attribute("type")) {
    info->suffix = type_attrib;
  }
  if (const char *alias_attrib = e->Attribute("alias")) {
    info->alias = alias_attrib;
  }
  if (const char *api_attrib = e->Attribute("api")) {
    info->api = api_attrib;
  }
}

void populateEntity(GroupInfo *info,
                    const tinyxml2::XMLElement *e,
                    const internal::EntityMap<EnumerantInfo> &map,
                    const std::string &api_name) {
  info->name = e->Attribute("name");
  FAIL_IF(info->name.empty(),
          "Group missing \"name\" attribute on line %d\n",
          e->GetLineNum());
  FOR_EACH_CHILD_ELEM_NAMED("enum", e, enum_ref) {
    const char *ref_name = enum_ref->Attribute("name");
    FAIL_IF(ref_name == nullptr,
            "Enum reference missing name attribute on line %d\n",
            enum_ref->GetLineNum());
    auto enum_it = map.find(ref_name);
    FAIL_IF(enum_it == map.end(),
            "Reference to undefined enum %s on line %d\n",
            ref_name,
            enum_ref->GetLineNum());
    const EnumerantInfo *enum_info =
        enum_it->second.get(api_name.c_str());
    FAIL_IF(enum_info == nullptr,
            "Failed to find enum %s for api %s\n",
            ref_name, api_name.c_str());
    info->enums.push_back(enum_info);
  }
}

void populateEntity(CommandInfo::ParamInfo *info,
                    const tinyxml2::XMLElement *e) {
  if (const char *group_attr = e->Attribute("group")) {
    info->group = group_attr;
  }
  if (const char *len_attr = e->Attribute("len")) {
    info->len = len_attr;
  }
  FOR_EACH_CHILD(e, child) {
    if (const tinyxml2::XMLText *text = child->ToText()) {
      info->ctype += text->Value();
    } else if (const tinyxml2::XMLElement *elem = child->ToElement()) {
      const char *tag_name = elem->Value();
      if (strcmp(tag_name, "ptype") == 0) {
        info->referenced_api_type = elem->GetText();
        info->ctype += info->referenced_api_type;
      } else if (strcmp(tag_name, "name") == 0) {
        info->name = elem->GetText();
        
      } else {
        FAIL("Unknown tag \"%s\" on line %d\n",
             tag_name,
             elem->GetLineNum());
      }
    }
  }
}

void populateEntity(CommandInfo *info,
                    const tinyxml2::XMLElement *e) {
  const tinyxml2::XMLElement *prototype_elem = e->FirstChildElement("proto");
  FOR_EACH_CHILD(prototype_elem, child) {
    if (const tinyxml2::XMLText *text = child->ToText()) {
      info->return_ctype += std::string(" ") + text->Value();
      info->prototype += text->Value();
    } else if (const tinyxml2::XMLElement *elem = child->ToElement()) {
      const char *tag_name = elem->Value();
      if (strcmp(tag_name, "ptype") == 0) {
        info->return_ctype += std::string(" ") + elem->GetText();
        info->referenced_api_type = elem->GetText();
        info->prototype += elem->GetText();
      } else if (strcmp(tag_name, "name") == 0) {
        info->name = elem->GetText();
        info->prototype += info->name;
      } else {
        FAIL("Unknown tag \"%s\" on line %d\n",
             tag_name,
             elem->GetLineNum());
      }
    }

    // Clean up return ctype.
    std::string &return_ctype = info->return_ctype;
    auto l = [](int c) { return !isspace(c); };
    return_ctype.erase(return_ctype.begin(), std::find_if(return_ctype.begin(),
                                                          return_ctype.end(),
                                                          l ));
    return_ctype.erase(std::find_if(return_ctype.rbegin(),
                                    return_ctype.rend(),
                                    l).base(), return_ctype.end());
  }
  FOR_EACH_CHILD_ELEM_NAMED("param", e, param) {
    CommandInfo::ParamInfo param_info;
    populateEntity(&param_info, param);
    info->parameters.emplace_back(std::move(param_info));
  }
  if (const tinyxml2::XMLElement *alias_elem =
          e->FirstChildElement("alias")) {
    info->alias = alias_elem->Attribute("name");
  }
  if (const tinyxml2::XMLElement *vecequiv_elem =
          e->FirstChildElement("vecequiv")) {
    info->vecequiv = vecequiv_elem->Attribute("name");
  }
}

template <class T, class... Types>
void loadEntities(const tinyxml2::XMLElement *container,
                  const char *kind,
                  EntityMap<T> &map,
                  Types&... extra_args) {
  FOR_EACH_CHILD_ELEM_NAMED(kind, container, entity_element) {
    T entity_info;
    populateEntity(&entity_info, entity_element, extra_args...);
    map[entity_info.name].add(entity_info);
  }
}

// Convenience class for comparing API versions.
class ApiVersion {
public:
  ApiVersion() = default;
  explicit ApiVersion(const char *version_string) {
    static std::regex expr("^([0-9]+)\\.([0-9]+)$",
                           std::regex_constants::ECMAScript);
    std::cmatch match; 
    std::regex_match(version_string, match, expr);
    if (match.size() == 3) {
      ver_maj_ = atoi(match[1].str().c_str());
      ver_min_ = atoi(match[2].str().c_str());
      valid_ = true;
    }
  }

  bool operator<=(const ApiVersion &other) const {
    if (ver_maj_ < other.ver_maj_) {
      return true;
    } else if (ver_maj_ == other.ver_maj_) {
      return ver_min_ <= other.ver_min_;
    }
    return false;
  }
  
  bool operator>(const ApiVersion &other) const {
    return !(*this <= other);
  }

  bool valid() const { return valid_; }

  int maj() const { return ver_maj_; }
  int min() const { return ver_min_; }

private:
  int ver_maj_ = 0;
  int ver_min_ = 0;
  bool valid_ = false;
};

extern const char *source_preamble;
extern const char *header_preamble;

struct GenerationOptions {
  std::string registry_file_name;
  std::string api_name;
  ApiVersion api_version;
  std::string profile;
  OutputGenerator *generator;
  std::string filename;
  std::unordered_set<std::string> extensions;
};

void processOperations(
    const tinyxml2::XMLElement *op_list,
    const GenerationOptions &options,
    const EntityMap<CommandInfo> &command_map,
    std::unordered_map<std::string,
                       std::unordered_set<std::string>> &entity_sets) {
  FOR_EACH_CHILD_ELEM(op_list, operation) {
    const char *profile_attrib = operation->Attribute("profile");
    if (profile_attrib &&
        strcmp(profile_attrib, options.profile.c_str()) != 0) {
      continue;
    }

    bool require = strcmp(operation->Value(), "require") == 0;
    FOR_EACH_CHILD_ELEM(operation, entity_ref) {
      const char *entity_type = entity_ref->Value();
      std::string name_attrib = entity_ref->Attribute("name");
      FAIL_IF(name_attrib.empty(),
              "%s missing name attribute on line %d\n",
              entity_type,
              entity_ref->GetLineNum());
      if (require) {
        entity_sets[entity_type].insert(name_attrib);
        if (strcmp(entity_type, "command") == 0) {
          // Types are (usually) not directly specified in the feature
          // element. They are supposed to be picked up transitively via 
          // command signatures. Same applies to groups.
          const CommandInfo *command =
            command_map.at(name_attrib).get(options.api_name.c_str());
          if (!command->referenced_api_type.empty()) {
            entity_sets["type"].insert(command->referenced_api_type);
          }
          for (const CommandInfo::ParamInfo &param : command->parameters) {
            if (!param.referenced_api_type.empty()) {
              entity_sets["type"].insert(param.referenced_api_type);
            }
            if(!param.group.empty()) {
              entity_sets["group"].insert(param.group);
            }
          }
        }
      } else {
        entity_sets[entity_type].erase(name_attrib);
      }
    }
  }
}

void generate(GenerationOptions &options) {
  // Load the registry file.
  tinyxml2::XMLDocument spec;
  FAIL_IF(spec.LoadFile(options.registry_file_name.c_str()) !=
             tinyxml2::XML_SUCCESS,
          "Failed to load file %s",
          options.registry_file_name.c_str());
  tinyxml2::XMLElement *root = spec.RootElement();
  
  EntityMap<TypeInfo> type_map;
  EntityMap<EnumerantInfo> enum_map;
  EntityMap<CommandInfo> command_map;
  EntityMap<GroupInfo> group_map;
  
  // Load information about API entities into the maps.
  loadEntities(root->FirstChildElement("types"), "type", type_map);
  loadEntities(root->FirstChildElement("commands"), "command", command_map);
  FOR_EACH_CHILD_ELEM_NAMED("enums", root, enums_container) {
    loadEntities(enums_container, "enum", enum_map);
  }
  loadEntities(root->FirstChildElement("groups"),
               "group",
               group_map,
               enum_map,
               options.api_name);

  // Each API version is described in a "feature" element.
  // The contents of the tag specify the difference against the previous version
  // (i.e. which types/commands/enums were added or removed).
  // Therefore, to get the full description of an API version, we need to
  // process all feature elements up to and including the required version, in
  // order of increasing version.
  // Its is not guaranteed that the "feature" elements will appear in any
  //  particular order, so we sort them first.
  std::vector<const tinyxml2::XMLElement*> feature_elements;
  std::vector<ApiVersion> api_version_numbers;
  FOR_EACH_CHILD_ELEM_NAMED("feature", root, feature) {
    const char *feature_api = feature->Attribute("api");
    FAIL_IF(feature_api == nullptr,
            "Feature tag missing api attribute on line %d\n",
            feature->GetLineNum());
    if (strcmp(feature_api, options.api_name.c_str()) == 0) {
      feature->SetUserData((void*)api_version_numbers.size());
      api_version_numbers.push_back(ApiVersion(feature->Attribute("number")));
      feature_elements.push_back(feature);
    }
  }
  std::sort(feature_elements.begin(),
            feature_elements.end(),
            [&api_version_numbers](const tinyxml2::XMLElement *e1,
               const tinyxml2::XMLElement *e2) {
              return api_version_numbers[(size_t)e1->GetUserData()] <=
                     api_version_numbers[(size_t)e2->GetUserData()];
            });

  // Process API versions.
  std::unordered_map<std::string,
                     std::unordered_set<std::string>> entity_sets;
  for (const tinyxml2::XMLElement *feature_element : feature_elements) {
    const ApiVersion &v =
        api_version_numbers[(size_t)feature_element->GetUserData()];
    if (v > options.api_version) { break; }
    processOperations(feature_element, options, command_map, entity_sets);
  }

  // Process extensions.
  const tinyxml2::XMLElement *extension_list =
      root->FirstChildElement("extensions");
  FOR_EACH_CHILD_ELEM_NAMED("extension", extension_list, extension) {
    std::string extension_name = extension->Attribute("name");
    FAIL_IF(extension_name.empty(),
            "Extension missing \"name\" attribute on line %d\n",
            extension->GetLineNum());
    std::string supported_api = extension->Attribute("supported");
    FAIL_IF(supported_api.empty(),
            "Extension missing \"supported\" attribute on line %d\n",
            extension->GetLineNum());
    std::regex supported_api_regex("^" + supported_api + "$",
                                   std::regex_constants::ECMAScript);
    bool extension_supported =
        std::regex_match(options.api_name, supported_api_regex);
    bool extension_requested = options.extensions.count(extension_name) >= 1;
    if (extension_requested && extension_supported) {
      processOperations(extension, options, command_map, entity_sets);
      options.extensions.erase(extension_name);
    } else if (extension_requested) {
      fprintf(stderr,
              "WARNING: extension %s requested, but not supported by API %s\n",
              extension_name.c_str(), options.api_name.c_str());
    }
  }
  std::ostringstream remaining_extensions;
  std::copy(options.extensions.begin(), options.extensions.end(),
            std::ostream_iterator<std::string>(remaining_extensions, ", "));
  FAIL_IF(!remaining_extensions.str().empty(),
          "Invalid extensions specified: %s\n",
          remaining_extensions.str().c_str());
  options.generator->start(options.filename, options.api_name, options.profile,
                           options.api_version.maj(),
                           options.api_version.min());
  std::function<void(ApiEntity<TypeInfo>&)> output_type =
      [&](ApiEntity<TypeInfo> &type) {
        const TypeInfo *info = type.get(options.api_name.c_str());
        FAIL_IF(info == nullptr,
                "Couldn't find type for api %s\n",
                options.api_name.c_str());
        if (type.isProcessed()) {
          return;
        }
        if (!info->requires.empty()) {
          output_type(type_map[info->requires]);
        }
        options.generator->processType(*info);
        type.markProcessed();
      };

  // KLUDGE: GLDEBUGPROC depends on these types but doesn't declare them as
  //         dependencies in any visible way. So force-output them at the very
  //         beginning.
  //         See https://github.com/KhronosGroup/OpenGL-Registry/issues/160
  output_type(type_map["GLenum"]);
  output_type(type_map["GLuint"]);
  output_type(type_map["GLsizei"]);
  output_type(type_map["GLchar"]);
 
  const std::unordered_set<std::string> &types = entity_sets["type"];
  for (const auto &type_name : types) {
    auto type_it = type_map.find(type_name);
    FAIL_IF(type_it == type_map.end(),
            "Reference to undefined type %s\n",
            type_name.c_str());
    output_type(type_it->second);
  }

  const std::unordered_set<std::string> &groups = entity_sets["group"];
  for (const auto &group_name : groups) {
    auto group_it = group_map.find(group_name);
    if(group_it == group_map.end()) {
      // It is not an error to refer to a group that had not been defined
      // before (see readme section 7.3).
      continue;
    }
    const GroupInfo *info = group_it->second.get(options.api_name.c_str());
    FAIL_IF(info == nullptr,
            "Failed to find group %s for api %s\n",
            group_name.c_str(),
            options.api_name.c_str());
    options.generator->processEnumGroup(*info);
  }

  const std::unordered_set<std::string> &enums = entity_sets["enum"];
  for (const auto &enum_name : enums) {
    auto enum_it = enum_map.find(enum_name);
    FAIL_IF(enum_it == enum_map.end(),
            "Reference to undefined enumerant %s\n",
            enum_name.c_str());
    const EnumerantInfo *info = enum_it->second.get(options.api_name.c_str());
    FAIL_IF(info == nullptr,
            "Failed to find enumerant %s for api %s\n",
            enum_name.c_str(),
            options.api_name.c_str());
    options.generator->processEnumerant(*info);
  }
 
  const std::unordered_set<std::string> &commands = entity_sets["command"];
  for (const auto &command_name : commands) {
    auto command_it = command_map.find(command_name);
    FAIL_IF(command_it == command_map.end(),
            "Reference to undefined command %s\n",
            command_name.c_str());
    const CommandInfo *info = command_it->second.get(options.api_name.c_str());
    FAIL_IF(info == nullptr,
            "Failed to find command %s for api %s\n",
            command_name.c_str(),
            options.api_name.c_str());
    options.generator->processCommand(*info);
  }
  options.generator->end();
  printf("Generation finished successfully!\n");
}

extern const char *help_message;

using GeneratorMap =
    std::unordered_map<std::string, std::unique_ptr<OutputGenerator>>;

void createGenerators(GeneratorMap &g);

}
}

#if !defined(__EMSCRIPTEN__)
#define GALOGEN_MAIN int main(int argc, char **argv)
#else
#define GALOGEN_MAIN int jsmain(int argc, char **argv)
#endif

#if defined(__EMSCRIPTEN__)
extern "C" {
#endif
GALOGEN_MAIN {
  galogen::internal::GeneratorMap generators;
  galogen::internal::createGenerators(generators);

  galogen::internal::GenerationOptions options;
  options.api_name = "gl";
  options.api_version = galogen::internal::ApiVersion("4.0");
  options.profile = "compatibility";
  options.generator = nullptr;
  options.filename = "gl";

  if (argc <= 1) {
    printf("%s\n", galogen::internal::help_message);
  } else {
    options.registry_file_name = argv[1];
    if (options.registry_file_name[0] == '-' &&
        options.registry_file_name[1] == '-') {
      fprintf(stderr, "WARNING: First argument \"%s\" looks suspicious."
                      " Did you forget to specify a path to the XML registry"
                      " file?\n",
              argv[1]);
    }

    bool api_ver_specified = false;
    for (size_t i = 2; i < argc; ++i) {
      std::string arg = argv[i];
      if (i + 1 >= argc) {
        FAIL("Inavlid options\n");
      }
      std::string value = argv[++i];
      if (arg == "--api") {
        FAIL_IF(value != "gl" && value != "gles2" &&
                value != "gles1" && value != "glsc2",
                "Invalid API name %s\n", value.c_str());
        options.api_name = value;
      } else if (arg == "--ver") {
          api_ver_specified = true;
          options.api_version = galogen::internal::ApiVersion(value.c_str());
          FAIL_IF(!options.api_version.valid(),
                  "Invalid version \"%s\"\n",
                  value.c_str());
      } else if (arg == "--profile") {
        FAIL_IF(value != "core" && value != "compatibility",
                "Profile must be either \"core\" or \"compatibility\"\n");
        options.profile = value;
      } else if (arg == "--filename") {
        options.filename = value;
      } else if (arg == "--generator") {
        auto generator_it = generators.find(value);
        FAIL_IF(generator_it == generators.end(),
                "Invalid generator \"%s\" specified.\n", value.c_str());
        options.generator = generator_it->second.get();
      } else if (arg == "--exts") {
        std::istringstream stream(value);
        std::string extension_name;
        while (std::getline(stream, extension_name, ',')) {
          options.extensions.insert("GL_" + extension_name);
        }
      } else {
        FAIL("Unrecognized option: %s\n", arg.c_str());
      }
    }
    const std::unordered_map<std::string, std::string> default_api_versions {
      {"gl", "4.0"},
      {"gles1", "1.0"},
      {"gles2", "2.0"},
      {"glsc2", "2.0"}
    };
    if (!api_ver_specified) {
      options.api_version =
        galogen::internal::ApiVersion(
          default_api_versions.find(options.api_name)->second.c_str());
    }
    if (options.generator == nullptr) {
      options.generator = generators["c_noload"].get();
    }
    generate(options);
  }
  return 0;
}
#if defined(__EMSCRIPTEN__)
}
#endif

namespace galogen {
namespace internal {
class COutputGenerator : public OutputGenerator {
public:
  explicit COutputGenerator(bool null_driver = false) :
      null_driver_(null_driver) {}

  // Invoked at the very start of output generation. This is where you should
  // do any setup, such as opening output files.
  void start(const std::string &name,
             const std::string &api_name,
             const std::string &api_profile,
             int api_ver_maj,
             int api_ver_min) override {
    output_h_ = fopen((name + ".h").c_str(), "w");
    output_c_ = fopen((name + ".c").c_str(), "w");
    FAIL_IF(output_h_ == nullptr || output_c_ == nullptr,
            "Failed to create output files\n");
    fprintf(output_h_, "%s\n", header_preamble);
    fprintf(output_h_,
            "#define GALOGEN_API_NAME \"%s\"\n"
            "#define GALOGEN_API_PROFILE \"%s\"\n"
            "#define GALOGEN_API_VER_MAJ %d\n"
            "#define GALOGEN_API_VER_MIN %d\n",
            api_name.c_str(), api_profile.c_str(),
            api_ver_maj, api_ver_min);
    fprintf(output_c_, "#include \"%s.h\"\n", name.c_str());
    if(!null_driver_) {
      fprintf(output_c_, "%s\n", source_preamble);
    }
  }

  void processType(const TypeInfo &type) override {
    fprintf(output_h_, "%s\n", type.type_cdecl.c_str());
  }

  void processEnumerant(const EnumerantInfo &enumerant) override {
    fprintf(output_h_, "#define %s %s%s\n",
            enumerant.name.c_str(),
            enumerant.value.c_str(),
            enumerant.suffix.c_str());
    if (!enumerant.alias.empty()) {
      fprintf(output_h_, "#define %s %s%s\n",
             enumerant.alias.c_str(),
             enumerant.value.c_str(),
             enumerant.suffix.c_str());
    }
  }

  void processCommand(const CommandInfo &command) override {
    // Build parameter list strings.
    std::string parameter_list_sig, parameter_list_call;
    for (const CommandInfo::ParamInfo &param : command.parameters) {
      if (!parameter_list_call.empty() && !parameter_list_sig.empty()) {
        parameter_list_sig += ", ";
        parameter_list_call += ", ";
      }
      parameter_list_sig += param.ctype + " " + param.name;
      parameter_list_call += param.name;
    }

    // Output function pointer declaration to header.
    fprintf(output_h_, // Function pointer type.
            "\ntypedef %s (GL_APIENTRY *PFN_%s)(%s);\n",
            command.return_ctype.c_str(),
            command.name.c_str(),
            parameter_list_sig.c_str());
    fprintf(output_h_, // Declaration.
            "extern PFN_%s _glptr_%s;\n",
            command.name.c_str(),
            command.name.c_str());

    // Add a macro that defines the command name to call the function pointer.
    fprintf(output_h_, "#define %s _glptr_%s\n",
            command.name.c_str(),
            command.name.c_str());
    if (!command.alias.empty()) {
      fprintf(output_h_,
              "#define %s %s\n",
              command.alias.c_str(),
              command.name.c_str());
    }

    // Output loader function to .c file.
    fprintf(output_c_, // Signature.
            "static %s GL_APIENTRY _impl_%s (%s) {\n",
            command.return_ctype.c_str(),
            command.name.c_str(),
            parameter_list_sig.c_str());
    if (null_driver_) {
      if (command.return_ctype != "void") {
        fprintf(output_c_,
                "  return (%s)0;\n", command.return_ctype.c_str());
      }
      fprintf(output_c_, "}\n");
    } else {
      fprintf(output_c_, // Implementation.
              "  _glptr_%s = (PFN_%s)GalogenGetProcAddress(\"%s\");\n  ",
              command.name.c_str(),
              command.name.c_str(),
              command.name.c_str());
      fprintf(output_c_,
              "%s _glptr_%s(%s);\n}\n",
              command.return_ctype != "void" ? "return" : "",
              command.name.c_str(),
              parameter_list_call.c_str());
    }
    fprintf(output_c_, // Definition of the function pointer.
            "PFN_%s _glptr_%s = _impl_%s;\n\n",
            command.name.c_str(),
            command.name.c_str(),
            command.name.c_str());
  }
  
  // Invoked at the end of output generation.
  void end() override {
    fprintf(output_h_, "#if defined(__cplusplus)\n}\n#endif\n");
    fprintf(output_h_, "#endif\n");
    fclose(output_h_);
    fclose(output_c_);
  }
  
private:
  FILE *output_h_;
  FILE *output_c_;
  bool null_driver_ = false;
};

const char *help_message = R"STR(
Galogen v. 1.0
===============
Galogen generates code to load OpenGL entry points  for the exact API version, 
profile and extensions that you specify.

Usage:
  galogen <path to GL registry XML file> [options] 

  --api - API name, such as gl or gles2. Default is gl.
Options:
  --ver - API version. Default is 4.0.
  --profile - Which API profile to generate the loader for. Allowed values are "core" and "compatibility". Default is "core".
  --exts - A comma-separated list of extensions. Default is empty. 
  --filename - Name for generated files (<api>_<ver>_<profile> by default). 
  --generator - Which generator to use. Default is "c_noload". 
  
Example:
  ./galogen gl.xml --api gl --ver 4.5 --profile core --filename gl
)STR";

const char *header_preamble = R"STR(
/* This file was auto-generated by Galogen */
#ifndef _GALOGEN_HEADER_
#define _GALOGEN_HEADER_
#if defined(__gl_h_) || defined(__GL_H__) || defined(__glext_h_) || defined(__GLEXT_H_) || defined(__gltypes_h_) || defined(__glcorearb_h_) || defined(__gl_glcorearb_h)
#error Galogen-generated header included after a GL header.
#endif

#define __gl_h_ 1
#define __gl32_h_ 1
#define __gl31_h_ 1
#define __GL_H__ 1
#define __glext_h_ 1
#define __GLEXT_H_ 1
#define __gltypes_h_ 1
#define __glcorearb_h_ 1
#define __gl_glcorearb_h_ 1

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define GL_APIENTRY APIENTRY
#else
#define GL_APIENTRY
#endif

#if defined(__cplusplus)
extern "C" {
#endif
)STR";

const char *source_preamble = R"STR(
/* This file was auto-generated by Galogen */
#include <assert.h>
#if defined(_WIN32)
void* GalogenGetProcAddress(const char *name) {
  static HMODULE opengl32module = NULL;
  static PROC(WINAPI *wgl_get_proc_address)(LPCSTR name) = NULL;
  if (!wgl_get_proc_address) {
    if (!opengl32module) {
      opengl32module = LoadLibraryA("opengl32.dll");
    }
    wgl_get_proc_address = (PROC(WINAPI*)(LPCSTR))GetProcAddress(opengl32module, "wglGetProcAddress");
    assert(wgl_get_proc_address);
  }
  void *ptr = (void *)wgl_get_proc_address(name);
  if(ptr == 0 || (ptr == (void*)1) || (ptr == (void*)2) || (ptr == (void*)3) ||
     (ptr == (void*)-1) ) {
    if (opengl32module == NULL) {
      opengl32module = LoadLibraryA("opengl32.dll");
      assert(opengl32module);
    }
    ptr = (void *)GetProcAddress(opengl32module, name);
  }
  return ptr;
}

#elif defined(__APPLE__)
#include <dlfcn.h>

static void* GalogenGetProcAddress (const char *name)
{
  static void* lib = NULL;
  if (NULL == lib)
    lib = dlopen(
      "/System/Library/Frameworks/OpenGL.framework/Versions/Current/OpenGL",
      RTLD_LAZY);
  return lib ? dlsym(lib, name) : NULL;
}
#elif defined(__ANDROID__)
#include <dlfcn.h>
#if GALOGEN_API_VER_MAJ == 3
#define GALOGEN_GLES_LIB "libGLESv3.so"
#elif GALOGEN_API_VER_MAJ == 2
#define GALOGEN_GLES_LIB "libGLESv2.so"
#else
#define GALOGEN_GLES_LIB "libGLESv1_CM.so"
#endif
static void* GalogenGetProcAddress(const char *name)
{
  static void* lib = NULL;
  if (NULL == lib) {
    lib = dlopen(GALOGEN_GLES_LIB, RTLD_LAZY);
    assert(lib);
  }
  return lib ? dlsym(lib, name) : NULL;
}

#else

#include <GL/glx.h>
#define GalogenGetProcAddress(name) (*glXGetProcAddressARB)((const GLubyte*)name)

#endif

)STR";

void createGenerators(GeneratorMap &g) {
  g["c_noload"] = std::unique_ptr<OutputGenerator>(new COutputGenerator());
  g["c_nulldriver"] =
      std::unique_ptr<OutputGenerator>(new COutputGenerator(true));
}

}
}
