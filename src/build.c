/* tsc
 * Copyright (c) 2021 Niels Sonnich Poulsen (http://nielssp.dk)
 * Licensed under the MIT license.
 * See the LICENSE file or http://opensource.org/licenses/MIT for more information.
 */

#include "build.h"

#include "collections.h"
#include "contentmap.h"
#include "core.h"
#include "datetime.h"
#include "html.h"
#include "images.h"
#include "interpreter.h"
#include "markdown.h"
#include "parser.h"
#include "reader.h"
#include "sitemap.h"
#include "strings.h"
#include "template.h"

#include <errno.h>
#include <getopt.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
  char *src_root;
  char *dist_root;
  SymbolMap *symbol_map;
  ModuleMap *modules;
} BuildInfo;

static void import_build_info(BuildInfo *build_info, Env *env) {
  env_def("SRC_ROOT", copy_c_string(build_info->src_root, env->arena), env);
  env_def("DIST_ROOT", copy_c_string(build_info->dist_root, env->arena), env);
}

Module *get_template(const char *name, Env *env) {
  Module *m = get_module(name, env->modules);
  if (m) {
    return m;
  }
  FILE *file = fopen(name, "r");
  if (!file) {
    fprintf(stderr, SGR_BOLD "%s: " ERROR_LABEL "%s" SGR_RESET "\n", name, strerror(errno));
    return NULL;
  }
  Reader *reader = open_reader(file, name, env->symbol_map);
  TokenStream tokens = read_all(reader, 1);
  if (reader_errors(reader)) {
    close_reader(reader);
    fclose(file);
    return NULL;
  }
  m = parse(tokens, name);
  close_reader(reader);
  fclose(file);
  if (m->parse_error) {
    delete_module(m);
    return NULL;
  }
  add_module(m, env->modules);
  return m;
}

Env *create_template_env(Value data, Env *parent) {
  Arena *arena = create_arena();
  Env *env = create_env(arena, parent->modules, parent->symbol_map);
  import_core(env);
  import_strings(env);
  import_collections(env);
  import_datetime(env);
  import_contentmap(env);
  import_template(env);
  import_html(env);
  import_images(env);
  import_markdown(env);
  if (data.type == V_OBJECT) {
    ObjectIterator it = iterate_object(data.object_value);
    Value entry_key, entry_value;
    while (object_iterator_next(&it, &entry_key, &entry_value)) {
      if (entry_key.type == V_SYMBOL) {
        env_put(entry_key.symbol_value, copy_value(entry_value, arena), env);
      }
    }
  }
  Value global;
  if (env_get_symbol("GLOBAL", &global, parent) && global.type == V_OBJECT) {
    ObjectIterator it = iterate_object(global.object_value);
    Value entry_key, entry_value;
    while (object_iterator_next(&it, &entry_key, &entry_value)) {
      if (entry_key.type == V_SYMBOL) {
        env_put(entry_key.symbol_value, copy_value(entry_value, env->arena), env);
      }
    }
    env_def("GLOBAL", copy_value(global, env->arena), env);
  }
  Value src_root, dist_root;
  if (env_get_symbol("SRC_ROOT", &src_root, parent)) {
    env_def("SRC_ROOT", copy_value(src_root, env->arena), env);
  }
  if (env_get_symbol("DIST_ROOT", &dist_root, parent)) {
    env_def("DIST_ROOT", copy_value(dist_root, env->arena), env);
  }
  return env;
}

void delete_template_env(Env *env) {
  delete_arena(env->arena);
}

Value eval_template(Module *module, Value data, Env *env) {
  env_def("FILE", copy_c_string(module->file_name, env->arena), env);
  char *file_name_copy = copy_string(module->file_name);
  char *dir = copy_string(dirname(file_name_copy));
  env_def("DIR", copy_c_string(dir, env->arena), env);
  free(file_name_copy);
  Value content = interpret(*module->root, env);
  Value layout;
  if (env_get_symbol("LAYOUT", &layout, env) && layout.type == V_STRING) {
    env_def("CONTENT", content, env);
    env_def("LAYOUT", nil_value, env);
    char *layout_name = string_to_c_string(layout.string_value);
    char *layout_path = combine_paths(dir, layout_name);
    Module *layout_module = get_template(layout_path, env);
    if (layout_module) {
      content = eval_template(layout_module, nil_value, env);
    }
    free(layout_path);
    free(layout_name);
  }
  free(dir);
  return content;
}

static int eval_script(FILE *file, const char *file_name, BuildInfo *build_info) {
  Reader *reader = open_reader(file, file_name, build_info->symbol_map);
  TokenStream tokens = read_all(reader, 0);
  if (reader_errors(reader)) {
    close_reader(reader);
    return 0;
  }
  Module *module = parse(tokens, file_name);
  close_reader(reader);
  if (module->parse_error) {
    delete_module(module);
    return 0;
  }
  add_module(module, build_info->modules);

  Arena *arena = create_arena();
  Env *env = create_env(arena, build_info->modules, build_info->symbol_map);
  import_core(env);
  import_strings(env);
  import_collections(env);
  import_datetime(env);
  import_sitemap(env);
  import_contentmap(env);
  import_markdown(env);
  import_build_info(build_info, env);
  env_def("FILE", copy_c_string(file_name, env->arena), env);
  char *file_name_copy = copy_string(file_name);
  char *dir = dirname(file_name_copy);
  env_def("DIR", copy_c_string(dir, env->arena), env);
  free(file_name_copy);
  interpret(*module->root, env);
  delete_arena(arena);
  return 1;
}

char *get_src_path(String *path, Env *env) {
  char *dir = get_env_string("DIR", env);
  if (!dir) {
    env_error(env, -1, "missing or invalid DIR");
    return NULL;
  }
  char *path_str = string_to_c_string(path);
  char *combined = combine_paths(dir, path_str);
  free(path_str);
  free(dir);
  return combined;
}

char *get_dist_path(String *path, Env *env) {
  char *dir = get_env_string("DIST_ROOT", env);
  if (!dir) {
    env_error(env, -1, "missing or invalid DIST_ROOT");
    return NULL;
  }
  char *path_str = string_to_c_string(path);
  char *combined = combine_paths(dir, path_str);
  free(path_str);
  free(dir);
  return combined;
}

static Value path_to_web_path(const Path *path, Arena *arena) {
  Value web_path = allocate_string(path->size, arena);
#if defined(_WIN32)
  for (int32_t i = 0; i < path->size; i++) {
    if (path->path[i] == PATH_SEP) {
      web_path.string_value->bytes[i] = '/';
    } else {
      web_path.string_value->bytes[i] = path->path[i];
    }
  }
#else
  memcpy(web_path.string_value->bytes, path->path, path->size);
#endif
  return web_path;
}

Value get_web_path(const Path *path, int absolute, Env *env) {
  if (!path_is_descending(path)) {
    return copy_c_string("#invalid-path", env->arena);
  }
  String *web_path;
  if (strcmp(path_get_name(path), "index.html") == 0) {
    Path *dir = path_get_parent(path);
    web_path = path_to_web_path(dir, env->arena).string_value;
    delete_path(dir);
  } else {
    web_path = path_to_web_path(path, env->arena).string_value;
  }
  Value root_value;
  String *root = NULL;
  if (absolute) {
    if (env_get(get_symbol("ROOT_URL", env->symbol_map), &root_value, env) && root_value.type == V_STRING) {
      root = root_value.string_value;
    }
  } else if (env_get(get_symbol("ROOT_PATH", env->symbol_map), &root_value, env) && root_value.type == V_STRING) {
    root = root_value.string_value;
  }
  if (web_path->size) {
    StringBuffer buffer = create_string_buffer((root ? root->size : 0) + web_path->size + 1, env->arena);
    if (root) {
      string_buffer_append(&buffer, root);
    }
    if (!buffer.string->size || buffer.string->bytes[buffer.string->size - 1] != '/') {
      string_buffer_put(&buffer, '/');
    }
    if (web_path->bytes[0] != '/') {
      string_buffer_append(&buffer, web_path);
    } else if (web_path->size > 1) {
      string_buffer_append_bytes(&buffer, web_path->bytes + 1, web_path->size - 1);
    }
    return finalize_string_buffer(buffer);
  } else if (root) {
    return (Value) { .type = V_STRING, .string_value = root };
  } else {
    return copy_c_string("/", env->arena);
  }
}

Path *get_src_root(Env *env) {
  Value value;
  if (env_get_symbol("SRC_ROOT", &value, env) && value.type == V_STRING) {
    return create_path((char *) value.string_value->bytes, value.string_value->size);
  }
  return NULL;
}

Path *get_dist_root(Env *env) {
  Value value;
  if (env_get_symbol("DIST_ROOT", &value, env) && value.type == V_STRING) {
    return create_path((char *) value.string_value->bytes, value.string_value->size);
  }
  return NULL;
}

int copy_asset(const Path *src, const Path *dest) {
  int result = 0;
  Path *dest_dir = path_get_parent(dest);
  if (mkdir_rec(dest_dir->path)) {
    result = copy_file(src->path, dest->path);
  }
  delete_path(dest_dir);
  return result;
}

int build(GlobalArgs args) {
  char *index_name = "index.tss";
  size_t name_length = strlen(index_name);
  char *cwd = getcwd(NULL, 0);
  size_t length = strlen(cwd);
  char *index_path = allocate(length + 1 + name_length + 1);
  memcpy(index_path, cwd, length);
  index_path[length] = PATH_SEP;
  memcpy(index_path + length + 1, index_name, name_length + 1);
  FILE *index = fopen(index_path, "r");
  if (!index) {
    do {
      length--;
      if (cwd[length] == PATH_SEP) {
        memcpy(index_path + length + 1, index_name, name_length + 1);
        index = fopen(index_path, "r");
        if (index) {
          cwd[length] = '\0';
          break;
        }
      }
    } while (length > 0);
  }
  if (index) {
    fprintf(stderr, INFO_LABEL "building %s" SGR_RESET "\n", index_path);
    BuildInfo build_info;
    build_info.src_root = cwd;
    build_info.dist_root = combine_paths(cwd, "dist");
    if (mkdir_rec(build_info.dist_root)) {
      build_info.symbol_map = create_symbol_map();
      build_info.modules = create_module_map();
      eval_script(index, index_path, &build_info);
      delete_symbol_map(build_info.symbol_map);
      delete_module_map(build_info.modules);
    }
    free(build_info.dist_root);
    fclose(index);
  } else {
    fprintf(stderr, ERROR_LABEL "index.tss not found" SGR_RESET "\n");
  }
  free(index_path);
  free(cwd);
  return 0;
}
