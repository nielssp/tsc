/* Plet
 * Copyright (c) 2021 Niels Sonnich Poulsen (http://nielssp.dk)
 * Licensed under the MIT license.
 * See the LICENSE file or http://opensource.org/licenses/MIT for more information.
 */

#include "html.h"

#include "build.h"
#include "inttypes.h"
#include "sitemap.h"
#include "strings.h"
#include "template.h"

#include <string.h>

#ifdef WITH_GUMBO
#include <gumbo.h>
#endif

static void html_encode_byte(StringBuffer *buffer, uint8_t byte, int quotes) {
  switch (byte) {
    case '&':
      string_buffer_printf(buffer, "&amp;");
      break;
    case '"':
      if (quotes) {
        string_buffer_printf(buffer, "&quot;");
      } else {
        string_buffer_put(buffer, byte);
      }
      break;
    case '\'':
      if (quotes) {
        string_buffer_printf(buffer, "&#39;");
      } else {
        string_buffer_put(buffer, byte);
      }
      break;
    case '<':
      string_buffer_printf(buffer, "&lt;");
      break;
    case '>':
      string_buffer_printf(buffer, "&gt;");
      break;
    default:
      string_buffer_put(buffer, byte);
      break;
  }
}

static Value h(const Tuple *args, Env *env) {
  check_args(1, args, env);
  Value value = args->values[0];
  StringBuffer buffer = create_string_buffer(0, env->arena);
  switch (value.type) {
    case V_SYMBOL:
      while (*value.symbol_value) {
        html_encode_byte(&buffer, *value.symbol_value, 1);
        value.symbol_value++;
      }
      break;
    case V_STRING:
      for (size_t i = 0; i < value.string_value->size; i++) {
        html_encode_byte(&buffer, value.string_value->bytes[i], 1);
      }
      break;
    default:
      string_buffer_append_value(&buffer, value);
      break;
  }
  return finalize_string_buffer(buffer);
}

static Value href(const Tuple *args, Env *env) {
  check_args_between(0, 2, args, env);
  Value path;
  Value class = create_string(NULL, 0, env->arena);
  if (args->size > 0) {
    path = args->values[0];
    if (path.type != V_STRING) {
      arg_type_error(0, V_STRING, args, env);
      return nil_value;
    }
    if (args->size > 1) {
      class = args->values[1];
      if (class.type != V_STRING) {
        arg_type_error(1, V_STRING, args, env);
        return nil_value;
      }
    }
  } else if (!env_get(get_symbol("PATH", env->symbol_map), &path, env) || path.type != V_STRING) {
    env_error(env, -1, "PATH is not set or not a string");
    return nil_value;
  }
  if (string_equals("index.html", path.string_value)) {
    path = copy_c_string("", env->arena);
  } else if (string_ends_with("/index.html", path.string_value)) {
    path = create_string(path.string_value->bytes, path.string_value->size - 11, env->arena);
  }
  if (path_is_current(path.string_value, env)) {
    if (class.string_value->size) {
      StringBuffer buffer = create_string_buffer(class.string_value->size + 8, env->arena);
      string_buffer_append(&buffer, class.string_value);
      string_buffer_printf(&buffer, " current");
      class = finalize_string_buffer(buffer);
    } else {
      class = copy_c_string("current", env->arena);
    }
  }
  Value root_path;
  if (env_get(get_symbol("ROOT_PATH", env->symbol_map), &root_path, env) && root_path.type == V_STRING) {
    path = combine_string_paths(root_path.string_value, path.string_value, env);
  }
  StringBuffer buffer = create_string_buffer(0, env->arena);
  string_buffer_printf(&buffer, " href=\"");
  for (size_t i = 0; i < path.string_value->size; i++) {
    html_encode_byte(&buffer, path.string_value->bytes[i], 1);
  }
  string_buffer_printf(&buffer, "\"");
  if (class.string_value->size) {
    string_buffer_printf(&buffer, " class=\"");
    for (size_t i = 0; i < class.string_value->size; i++) {
      html_encode_byte(&buffer, class.string_value->bytes[i], 1);
    }
    string_buffer_printf(&buffer, "\"");
  }
  return finalize_string_buffer(buffer);
}

static void html_to_string(Value node, StringBuffer *buffer, Env *env) {
  if (node.type == V_OBJECT) {
    Value tag = nil_value;
    object_get(node.object_value, create_symbol(get_symbol("tag", env->symbol_map)), &tag);
    if (tag.type == V_SYMBOL) {
      string_buffer_put(buffer, '<');
      string_buffer_printf(buffer, "%s", tag.symbol_value);
      Value attributes;
      if (object_get(node.object_value, create_symbol(get_symbol("attributes", env->symbol_map)),
            &attributes) && attributes.type == V_OBJECT) {
        ObjectIterator it = iterate_object(attributes.object_value);
        Value key, value;
        while (object_iterator_next(&it, &key, &value)) {
          if (key.type == V_SYMBOL && value.type == V_STRING) {
            string_buffer_put(buffer, ' ');
            string_buffer_printf(buffer, "%s", key.symbol_value);
            if (value.string_value->size) {
              string_buffer_put(buffer, '=');
              string_buffer_put(buffer, '"');
              for (size_t i = 0; i < value.string_value->size; i++) {
                html_encode_byte(buffer, value.string_value->bytes[i], 1);
              }
              string_buffer_put(buffer, '"');
            }
          }
        }
      }
      string_buffer_put(buffer, '>');
    }
    Value children;
    if (object_get(node.object_value, create_symbol(get_symbol("children", env->symbol_map)), &children)
        && children.type == V_ARRAY) {
      for (size_t i = 0; i < children.array_value->size; i++) {
        html_to_string(children.array_value->cells[i], buffer, env);
      }
    }
    Value self_closing = nil_value;
    object_get(node.object_value, create_symbol(get_symbol("self_closing", env->symbol_map)),
        &self_closing);
    if (tag.type == V_SYMBOL && !is_truthy(self_closing)) {
      string_buffer_put(buffer, '<');
      string_buffer_put(buffer, '/');
      string_buffer_printf(buffer, "%s", tag.symbol_value);
      string_buffer_put(buffer, '>');
    }
  } else if (node.type == V_STRING) {
    for (size_t i = 0; i < node.string_value->size; i++) {
      html_encode_byte(buffer, node.string_value->bytes[i], 0);
    }
  }
}

static Value html(const Tuple *args, Env *env) {
  check_args(1, args, env);
  StringBuffer output = create_string_buffer(0, env->arena);
  html_to_string(args->values[0], &output, env);
  return finalize_string_buffer(output);
}

static Value no_title(const Tuple *args, Env *env) {
  check_args(1, args, env);
  Value src = args->values[0];
  Value title_tag = html_find_tag(get_symbol("h1", env->symbol_map), src);
  if (title_tag.type == V_OBJECT) {
    html_remove_node(title_tag.object_value, src);
  }
  return src;
}

typedef struct {
  int absolute;
  Path *src_root;
  Path *dist_root;
  Path *asset_root;
  Object *reverse_paths;
  Env *env;
} LinkArgs;

static int transform_link(Value node, const char *attribute_name, LinkArgs *args) {
  Value src = html_get_attribute(node, attribute_name);
  if (src.type != V_STRING) {
    return 0;
  }
  if (string_starts_with("pletasset:", src.string_value)) {
    Path *asset_path = create_path((char *) src.string_value->bytes + sizeof("pletasset:") - 1,
        src.string_value->size - (sizeof("pletasset:") - 1));
    Path *src_path = path_join(args->src_root, asset_path, 1);
    Value src_path_string = create_string((uint8_t *) src_path->path, src_path->size, args->env->arena);
    Value reverse_path_value;
    if (object_get(args->reverse_paths, src_path_string, &reverse_path_value) && reverse_path_value.type == V_STRING) {
      Path *reverse_path = create_path((char *) reverse_path_value.string_value->bytes,
          reverse_path_value.string_value->size);
      html_set_attribute(node, attribute_name, get_web_path(reverse_path, args->absolute, args->env).string_value,
          args->env);
      delete_path(reverse_path);
    } else {
      Path *asset_web_path = path_join(args->asset_root, asset_path, 1);
      Path *dist_path = path_join(args->dist_root, asset_web_path, 1);
      if (copy_asset(src_path, dist_path)) {
        notify_output_observers(dist_path, args->env);
      }
      html_set_attribute(node, attribute_name, get_web_path(asset_web_path, args->absolute, args->env).string_value,
          args->env);
      delete_path(dist_path);
      delete_path(asset_web_path);
    }
    delete_path(src_path);
    delete_path(asset_path);
  } else if (string_starts_with("pletlink:", src.string_value)) {
    Path *web_path = create_path((char *) src.string_value->bytes + sizeof("pletlink:") - 1,
        src.string_value->size - (sizeof("pletlink:") - 1));
    html_set_attribute(node, attribute_name, get_web_path(web_path, args->absolute, args->env).string_value,
        args->env);
    delete_path(web_path);
  }
  return 1;
}

static HtmlTransformation transform_links(Value node, void *context) {
  LinkArgs *args = context;
  if (!transform_link(node, "src", args)) {
    transform_link(node, "href", args);
  }
  return HTML_NO_ACTION;
}

static Value links_or_urls(Value src, int absolute, Env *env) {
  Path *src_root = get_src_root(env);
  if (src_root) {
    Path *dist_root = get_dist_root(env);
    if (dist_root) {
      Value reverse_paths;
      if (env_get_symbol("REVERSE_PATHS", &reverse_paths, env) && reverse_paths.type == V_OBJECT) {
        Path *asset_root = create_path("assets", -1);
        LinkArgs context = {absolute, src_root, dist_root, asset_root, reverse_paths.object_value, env};
        src = html_transform(src, transform_links, &context);
        delete_path(asset_root);
      } else {
        env_error(env, -1, "REVERSE_PATHS missing or not an object");
      }
      delete_path(dist_root);
    } else {
      env_error(env, -1, "DIST_ROOT missing or not a string");
    }
    delete_path(src_root);
  } else {
    env_error(env, -1, "SRC_ROOT missing or not a string");
  }
  return src;
}

static Value links(const Tuple *args, Env *env) {
  check_args(1, args, env);
  Value src = args->values[0];
  return links_or_urls(src, 0, env);
}

static Value urls(const Tuple *args, Env *env) {
  check_args(1, args, env);
  Value src = args->values[0];
  return links_or_urls(src, 1, env);
}

typedef struct {
  int after_split;
} ReadMoreArgs;

static HtmlTransformation transform_read_more(Value node, void *context) {
  ReadMoreArgs *args = context;
  if (args->after_split) {
    return HTML_REMOVE;
  }
  if (node.type == V_OBJECT) {
    Value comment;
    if (object_get_symbol(node.object_value, "comment", &comment) && comment.type == V_STRING) {
      if (string_equals("more", comment.string_value)) {
        args->after_split = 1;
        return HTML_REMOVE;
      }
    }
  }
  return HTML_NO_ACTION;
}

static Value read_more(const Tuple *args, Env *env) {
  check_args(1, args, env);
  Value src = args->values[0];
  ReadMoreArgs context = {0};
  src = html_transform(src, transform_read_more, &context);
  return src;
}

static Value text_content(const Tuple *args, Env *env) {
  check_args(1, args, env);
  Value src = args->values[0];
  StringBuffer buffer = create_string_buffer(0, env->arena);
  html_text_content(src, &buffer);
  return finalize_string_buffer(buffer);
}

#ifdef WITH_GUMBO
static Value parse_html(const Tuple *args, Env *env) {
  check_args(1, args, env);
  Value src = args->values[0];
  if (src.type != V_STRING) {
    arg_type_error(0, V_STRING, args, env);
    return nil_value;
  }
  return html_parse(src.string_value, env);
}
#endif

void import_html(Env *env) {
  env_def_fn("h", h, env);
  env_def_fn("href", href, env);
  env_def_fn("html", html, env);
  env_def_fn("no_title", no_title, env);
  env_def_fn("links", links, env);
  env_def_fn("urls", urls, env);
  env_def_fn("read_more", read_more, env);
  env_def_fn("text_content", text_content, env);
#ifdef WITH_GUMBO
  env_def_fn("parse_html", parse_html, env);
#endif
}

#ifdef WITH_GUMBO

static Value convert_gumbo_node(GumboNode *node, Env *env) {
  switch (node->type) {
    case GUMBO_NODE_DOCUMENT: {
      Value obj = create_object(2, env->arena);
      object_def(obj.object_value, "type", create_symbol(get_symbol("document", env->symbol_map)), env);
      Value children = create_array(node->v.element.children.length, env->arena);
      for (unsigned int i = 0; i < node->v.element.children.length; i++) {
        Value child = convert_gumbo_node(node->v.element.children.data[i], env);
        if (child.type != V_NIL) {
          array_push(children.array_value, child, env->arena);
        }
      }
      object_def(obj.object_value, "children", children, env);
      object_def(obj.object_value, "line", create_int(node->v.element.start_pos.line), env);
      return obj;
    }
    case GUMBO_NODE_ELEMENT: {
      Value obj = create_object(4, env->arena);
      object_def(obj.object_value, "type", create_symbol(get_symbol("element", env->symbol_map)), env);
      object_def(obj.object_value, "tag",
          create_symbol(get_symbol(gumbo_normalized_tagname(node->v.element.tag), env->symbol_map)),
          env);
      Value attributes = create_object(node->v.element.attributes.length, env->arena);
      for (unsigned int i = 0; i < node->v.element.attributes.length; i++) {
        GumboAttribute *attribute = node->v.element.attributes.data[i];
        object_put(attributes.object_value, create_symbol(get_symbol(attribute->name, env->symbol_map)),
            copy_c_string(attribute->value, env->arena), env->arena);
      }
      object_def(obj.object_value, "attributes", attributes, env);
      Value children = create_array(node->v.element.children.length, env->arena);
      for (unsigned int i = 0; i < node->v.element.children.length; i++) {
        Value child = convert_gumbo_node(node->v.element.children.data[i], env);
        if (child.type != V_NIL) {
          array_push(children.array_value, child, env->arena);
        }
      }
      object_def(obj.object_value, "children", children, env);
      object_def(obj.object_value, "self_closing",
          node->v.element.original_end_tag.length == 0 ? true_value : false_value, env);
      object_def(obj.object_value, "line", create_int(node->v.element.start_pos.line), env);
      return obj;
    }
    case GUMBO_NODE_TEXT:
    case GUMBO_NODE_CDATA:
      return copy_c_string(node->v.text.text, env->arena);
    case GUMBO_NODE_COMMENT: {
      Value obj = create_object(2, env->arena);
      object_def(obj.object_value, "type", create_symbol(get_symbol("comment", env->symbol_map)), env);
      object_def(obj.object_value, "comment", copy_c_string(node->v.text.text, env->arena), env);
      object_def(obj.object_value, "line", create_int(node->v.text.start_pos.line), env);
      return obj;
    }
    case GUMBO_NODE_WHITESPACE:
      return copy_c_string(node->v.text.text, env->arena);
    case GUMBO_NODE_TEMPLATE:
    default:
      return nil_value;
  }
}

Value html_parse(String *html, Env *env) {
  GumboOptions options = kGumboDefaultOptions;
  options.fragment_context = GUMBO_TAG_DIV;
  GumboOutput *output = gumbo_parse_with_options(&options, (char *) html->bytes, html->size);
  Value root = convert_gumbo_node(output->root, env);
  gumbo_destroy_output(&options, output);
  if (root.type == V_OBJECT) {
      object_def(root.object_value, "type", create_symbol(get_symbol("fragment", env->symbol_map)), env);
      object_def(root.object_value, "tag", nil_value, env);
  }
  return root;
}

#else /* ifdef WITH_GUMBO */

Value html_parse(String *html, Env *env) {
  return nil_value;
}

#endif /* ifdef WITH_GUMBO */

void html_text_content(Value node, StringBuffer *buffer) {
  if (node.type == V_OBJECT) {
    Value children;
    if (object_get_symbol(node.object_value, "children", &children)) {
      if (children.type == V_ARRAY) {
        for (size_t i = 0; i < children.array_value->size; i++) {
          html_text_content(children.array_value->cells[i], buffer);
        }
      }
    }
  } else if (node.type == V_STRING) {
    string_buffer_append(buffer, node.string_value);
  }
}

Value html_find_tag(Symbol tag_name, Value node) {
  if (node.type == V_OBJECT) {
    Value node_tag;
    if (object_get_symbol(node.object_value, "tag", &node_tag)) {
      if (node_tag.type == V_SYMBOL && node_tag.symbol_value == tag_name) {
        return node;
      }
    }
    Value children;
    if (object_get_symbol(node.object_value, "children", &children)
        && children.type == V_ARRAY) {
      for (size_t i = 0; i < children.array_value->size; i++) {
        Value result = html_find_tag(tag_name, children.array_value->cells[i]);
        if (result.type != V_NIL) {
          return result;
        }
      }
    }
  }
  return nil_value;
}

int html_remove_node(Object *needle, Value haystack) {
  if (haystack.type == V_OBJECT) {
    if (haystack.object_value == needle) {
      return 1;
    }
    Value children;
    if (object_get_symbol(haystack.object_value, "children", &children)
        && children.type == V_ARRAY) {
      for (size_t i = 0; i < children.array_value->size; i++) {
        if (html_remove_node(needle, children.array_value->cells[i])) {
          array_remove(children.array_value, i);
          break;
        }
      }
    }
  }
  return 0;
}

static HtmlTransformation internal_html_transform(Value node, HtmlTransformation (*acceptor)(Value, void *),
    void *context);

static HtmlTransformation internal_html_transform(Value node, HtmlTransformation (*acceptor)(Value, void *),
    void *context) {
  HtmlTransformation transformation = acceptor(node, context);
  if (transformation.type != HT_NO_ACTION) {
    return transformation;
  }
  if (node.type == V_OBJECT) {
    Value children;
    if (object_get_symbol(node.object_value, "children", &children) && children.type == V_ARRAY) {
      for (size_t i = 0; i < children.array_value->size; i++) {
        HtmlTransformation child_ht = internal_html_transform(children.array_value->cells[i], acceptor,
            context);
        if (child_ht.type == HT_REMOVE) {
          array_remove(children.array_value, i);
          i--;
        } else if (child_ht.type == HT_REPLACE) {
          children.array_value->cells[i] = child_ht.replacement;
        }
      }
    }
  }
  return transformation;
}

Value html_transform(Value node, HtmlTransformation (*acceptor)(Value, void *), void *context) {
  HtmlTransformation transformation = internal_html_transform(node, acceptor, context);
  if (transformation.type == HT_REMOVE) {
    return nil_value;
  } else if (transformation.type == HT_REPLACE) {
    return transformation.replacement;
  } else {
    return node;
  }
}

int html_is_tag(Value node, const char *tag_name) {
  if (node.type == V_OBJECT) {
    Value node_tag;
    if (object_get_symbol(node.object_value, "tag", &node_tag)) {
      if (node_tag.type == V_SYMBOL && strcmp(node_tag.symbol_value, tag_name) == 0) {
        return 1;
      }
    }
  }
  return 0;
}

Value html_create_element(const char *tag_name, int self_closing, Env *env) {
  Value node = create_object(5, env->arena);
  object_def(node.object_value, "type", create_symbol(get_symbol("element", env->symbol_map)), env);
  object_def(node.object_value, "tag", create_symbol(get_symbol(tag_name, env->symbol_map)), env);
  Value attributes = create_object(0, env->arena);
  object_def(node.object_value, "attributes", attributes, env);
  Value children = create_array(0, env->arena);
  object_def(node.object_value, "children", children, env);
  object_def(node.object_value, "self_closing", self_closing ? true_value : false_value, env);
  return node;
}

void html_prepend_child(Value node, Value child, Arena *arena) {
  if (node.type == V_OBJECT) {
    Value children;
    if (object_get_symbol(node.object_value, "children", &children) && children.type == V_ARRAY) {
      array_unshift(children.array_value, child, arena);
    }
  }
}

void html_append_child(Value node, Value child, Arena *arena) {
  if (node.type == V_OBJECT) {
    Value children;
    if (object_get_symbol(node.object_value, "children", &children) && children.type == V_ARRAY) {
      array_push(children.array_value, child, arena);
    }
  }
}

Value html_get_attribute(Value node, const char *attribute_name) {
  if (node.type == V_OBJECT) {
    Value attributes;
    if (object_get_symbol(node.object_value, "attributes", &attributes) && attributes.type == V_OBJECT) {
      Value attribute;
      if (object_get_symbol(attributes.object_value, attribute_name, &attribute)) {
        return attribute;
      }
    }
  }
  return nil_value;
}

void html_set_attribute(Value node, const char *attribute_name, String *string_value, Env *env) {
  if (node.type == V_OBJECT) {
    Value attributes;
    if (object_get_symbol(node.object_value, "attributes", &attributes) && attributes.type == V_OBJECT) {
      Value value = (Value) { .type = V_STRING, .string_value = string_value };
      object_def(attributes.object_value, attribute_name, value, env);
    }
  }
}

void html_error(Value node, const Path *path, const char *format, ...) {
  va_list va;
  Value line;
  if (node.type == V_OBJECT && object_get_symbol(node.object_value, "line", &line) && line.type == V_INT) {
    fprintf(stderr, SGR_BOLD "%s:%" PRId64 ": " ERROR_LABEL, path->path, line.int_value);
  } else {
    fprintf(stderr, SGR_BOLD "%s: " ERROR_LABEL, path->path);
  }
  va_start(va, format);
  vfprintf(stderr, format, va);
  va_end(va);
  fprintf(stderr, SGR_RESET "\n");
}
