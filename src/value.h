/* Plet
 * Copyright (c) 2021 Niels Sonnich Poulsen (http://nielssp.dk)
 * Licensed under the MIT license.
 * See the LICENSE file or http://opensource.org/licenses/MIT for more information.
 */

#ifndef VALUE_H
#define VALUE_H

#include "ast.h"
#include "hashmap.h"

#include <stddef.h>
#include <stdint.h>
#include <time.h>

typedef struct Module Module;
typedef struct ModuleMap ModuleMap;

typedef struct Env Env;

typedef struct Value Value;
typedef struct String String;
typedef struct Array Array;
typedef struct Object Object;
typedef struct ObjectIterator ObjectIterator;
typedef struct Entry Entry;
typedef struct Closure Closure;


typedef struct Tuple Tuple;

#define ENV_ARG_ALL -1
#define ENV_ARG_NONE -2

typedef enum {
  ENV_ERROR,
  ENV_WARN,
  ENV_INFO
} EnvErrorLevel;

struct Env {
  Arena *arena;
  ModuleMap *modules;
  SymbolMap *symbol_map;
  Env *parent_env;
  Node *calling_node;
  char *error;
  int error_arg;
  EnvErrorLevel error_level;
  GenericHashMap global;
  Array *exports;
  int64_t loops;
};

typedef enum {
  M_SYSTEM,
  M_USER,
  M_DATA,
  M_ASSET
} ModuleType;

struct Module {
  ModuleType type;
  Path *file_name;
  time_t mtime;
  int dirty;
  union {
    struct {
      void (*import_func)(Env *);
    } system_value;
    struct {
      Node *root;
      int parse_error;
    } user_value;
    struct {
      Node *root;
      int parse_error;
    } data_value;
    struct {
      int width;
      int height;
    } asset_value;
  };
};

typedef enum {
  V_NIL,
  V_BOOL,
  V_INT,
  V_FLOAT,
  V_SYMBOL,
  V_STRING,
  V_ARRAY,
  V_OBJECT,
  V_TIME,
  V_FUNCTION,
  V_CLOSURE
} ValueType;

struct Value {
  ValueType type;
  union {
    int64_t int_value;
    double float_value;
    Symbol symbol_value;
    String *string_value;
    Array *array_value;
    Object *object_value;
    time_t time_value;
    Value (*function_value)(const Tuple *, Env *);
    Closure *closure_value;
  };
};

#define nil_value ((Value) { .type = V_NIL, .int_value = 0 })

#define true_value ((Value) { .type = V_BOOL, .int_value = 1 })

#define false_value ((Value) { .type = V_BOOL, .int_value = 0 })

#define create_int(i) ((Value) { .type = V_INT, .int_value = (i) })

#define create_float(f) ((Value) { .type = V_FLOAT, .float_value = (f) })

#define create_symbol(s) ((Value) { .type = V_SYMBOL, .symbol_value = (s) })

#define create_time(i) ((Value) { .type = V_TIME, .time_value = (i) })

struct String {
  size_t size;
  uint8_t bytes[];
};

struct Array {
  Value *cells;
  size_t capacity;
  size_t size;
};

struct ObjectIterator {
  Object *object;
  size_t next_index;
};

struct Entry {
  Value key;
  Value value;
};

struct Closure {
  NameList *params;
  Node body;
  NameList *free_variables;
  Env *env;
};

struct Tuple {
  size_t size;
  Value values[];
};

Env *create_env(Arena *arena, ModuleMap *modules, SymbolMap *symbol_map);

Env *create_child_env(Env *parent);

void env_put(Symbol name, Value value, Env *env);

#define env_def(name, value, env) env_put(get_symbol((name), (env)->symbol_map), (value), (env))

#define env_def_fn(name, func, env) \
  env_put(get_symbol((name), (env)->symbol_map), (Value) { .type = V_FUNCTION, .function_value = (func) }, (env))

#define env_export(name, env) \
  array_push((env)->exports, create_symbol(get_symbol((name), (env)->symbol_map)), (env)->arena)

#define check_args(length, args, env) \
  do {\
    if ((args)->size < (length)) {\
      env_error((env), ENV_ARG_ALL, "%s: too few arguments for function, %d expected", __func__, (length));\
      return nil_value;\
    } else if ((args)->size > (length)) {\
      env_error((env), (length), "%s: too many arguments for function, %d expected", __func__, (length));\
      return nil_value;\
    }\
  } while (0)

#define check_args_min(length, args, env) \
  do {\
    if ((args)->size < (length)) {\
      env_error((env), ENV_ARG_ALL, "%s: too few arguments for function, %d expected", __func__, (length));\
      return nil_value;\
    }\
  } while (0)

#define check_args_between(min, max, args, env) \
  do {\
    if ((args)->size < (min)) {\
      env_error((env), ENV_ARG_ALL, "%s: too few arguments for function, %d expected", __func__, (min));\
      return nil_value;\
    } else if ((args)->size > (max)) {\
      env_error((env), (max), "%s: too many arguments for function, %d expected", __func__, (max));\
      return nil_value;\
    }\
  } while (0)

#define arg_type_error(index, expected_type, args, env) \
  env_error((env), index, "%s: unexpected argument of type %s, %s expected", __func__, value_name(args->values[index].type), value_name(expected_type))\

#define arg_error(index, expected, args, env) \
  env_error((env), index, "%s: unexpected argument of type %s, %s expected", __func__, value_name(args->values[index].type), expected)\

int env_get(Symbol name, Value *value, Env *env);

int env_get_symbol(const char *name, Value *value, Env *env);

const String *get_env_string(const char *name, Env *env);

void display_env_error(Node node, EnvErrorLevel level, int show_line, const char *format, ...);

void env_error(Env *env, int arg, const char *format, ...);

void env_warn(Env *env, int arg, const char *format, ...);

void env_info(Env *env, int arg, const char *format, ...);

void env_clear_error(Env *env);

int equals(Value a, Value b);

int is_truthy(Value value);

Hash value_hash(Hash h, Value value);

Value copy_value(Value value, Env *env);

void value_to_string(Value value, Buffer *buffer);

const char *value_name(ValueType type);

Value create_string(const uint8_t *bytes, size_t size, Arena *arena);

Value path_to_string(const Path *path, Arena *arena);
Path *string_to_path(const String *string);

Value copy_c_string(const char *str, Arena *arena);

char *string_to_c_string(String *string);

Value allocate_string(size_t size, Arena *arena);

Value reallocate_string(String *string, size_t size, Arena *arena);

Value create_array(size_t capacity, Arena *arena);

void array_push(Array *array, Value elem, Arena *arena);

int array_pop(Array *array, Value *elem);

void array_unshift(Array *array, Value elem, Arena *arena);

int array_shift(Array *array, Value *elem);

int array_remove(Array *array, int index);

Value create_object(size_t capacity, Arena *arena);

void object_put(Object *object, Value key, Value value, Arena *arena);

#define object_def(object, name, value, env) \
  object_put((object), create_symbol(get_symbol((name), (env)->symbol_map)), (value), (env)->arena)

int object_get(Object *object, Value key, Value *value);

int object_get_symbol(Object *object, const char *key, Value *value);

int object_remove(Object *object, Value key, Value *value);

size_t object_size(Object *object);

ObjectIterator iterate_object(Object *object);

int object_iterator_next(ObjectIterator *it, Value *key, Value *value);

Value create_closure(NameList *params, NameList *free_variables, Node body, Env *env, Arena *arena);

#endif
