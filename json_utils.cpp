#include "stdafx.h"
#include "json_utils.hpp"
#include "utils.hpp"
#include "logger.hpp"

using namespace std;

//////////////////////////////////////////////////////////////////////////
// 
// JsonValue
//
//////////////////////////////////////////////////////////////////////////

JsonValue::JsonValuePtr JsonValue::emptyValue() {
  static JsonValue::JsonValuePtr obj;
  return obj;
}

JsonValue::JsonValue() : _type(JS_NULL) {}

JsonValue::JsonValue(JsonType type)
  : _type(type)
{
}

JsonValue::~JsonValue() {
  switch (_type) {
  case JS_STRING: SAFE_FREE(_string); break;
  }
}

JsonValue::JsonValuePtr JsonValue::create_string(const char *str) {
  auto js = JsonValuePtr(new JsonValue(JS_STRING));
  js->_string = strdup(str);
  return js;
}

JsonValue::JsonValuePtr JsonValue::create_string(const std::string &str) {
  return create_string(str.c_str());
}

JsonValue::JsonValuePtr JsonValue::create_number(double value) {
  auto js = JsonValuePtr(new JsonValue(JS_NUMBER));
  js->_number = value;
  return js;
}

JsonValue::JsonValuePtr JsonValue::create_int(int value) {
  auto js = JsonValuePtr(new JsonValue(JS_INT));
  js->_int = value;
  return js;
}

JsonValue::JsonValuePtr JsonValue::create_bool(bool value) {
  auto js = JsonValuePtr(new JsonValue(JS_BOOL));
  js->_bool = value;
  return js;
}

JsonValue::JsonValuePtr JsonValue::create_array() {
  return JsonValuePtr(new JsonArray());
}

JsonValue::JsonValuePtr JsonValue::create_object() {
  return JsonValuePtr(new JsonObject());
}

//////////////////////////////////////////////////////////////////////////
// 
// JsonObject
//
//////////////////////////////////////////////////////////////////////////

JsonObject::JsonObject() 
  : JsonValue(JS_OBJECT) 
{
}

JsonObject::JsonValuePtr JsonObject::add_key_value(const string &key, const JsonValuePtr &value) {
  return _key_value[key] = value;
}

JsonObject::JsonValuePtr JsonObject::add_key_value(const std::string &key, int value) {
  return _key_value[key] = JsonValue::create_int(value);
}

JsonObject::JsonValuePtr JsonObject::add_key_value(const std::string &key, uint32 value) {
  return _key_value[key] = JsonValue::create_int(value);
}

JsonObject::JsonValuePtr JsonObject::add_key_value(const std::string &key, double value) {
  return _key_value[key] = JsonValue::create_number(value);
}

JsonObject::JsonValuePtr JsonObject::add_key_value(const std::string &key, const std::string &value) {
  return _key_value[key] = JsonValue::create_string(value);
}

JsonObject::JsonValuePtr JsonObject::add_key_value(const std::string &key, bool value) {
  return _key_value[key] = JsonValue::create_bool(value);
}


JsonObject::JsonValuePtr JsonObject::operator[](const std::string &key) const { 
  auto it = _key_value.find(key);
  return it == _key_value.end() ? emptyValue() : it->second;
}

bool JsonObject::has_key(const std::string &key) const
{
  return _key_value.find(key) != _key_value.end();
}

//////////////////////////////////////////////////////////////////////////
// 
// JsonArray
//
//////////////////////////////////////////////////////////////////////////

JsonArray::JsonArray() 
  : JsonValue(JS_ARRAY) 
{
}

JsonObject::JsonValuePtr JsonArray::add_value(const JsonValuePtr &value) {
  _value.push_back(value);
  return value;
}


JsonValue::JsonValuePtr JsonArray::operator[](int idx) const 
{ 
  return _value[idx]; 
}

//////////////////////////////////////////////////////////////////////////
// 
// print_json
//
//////////////////////////////////////////////////////////////////////////

struct Buffer {
  Buffer(int initial_size) : ofs(0) {
    buf.resize(initial_size);
  }

  void add_char(char ch) {
    safety(32);
    buf[ofs++] = ch;
  }

  void add_int(int a) {
    safety(32);
    ofs += sprintf(&buf[ofs], "%d", a);
  }

  void add_number(double a) {
    safety(2048);
    ofs += sprintf(&buf[ofs], "%f", a);
  }

  void add_string(const string &str, bool quote) {
    safety(2 * str.size());
    if (quote)
      buf[ofs++] = '\"';
    memcpy(&buf[ofs], str.c_str(), str.size());
    ofs += str.size();
    if (quote)
      buf[ofs++] = '\"';
  }

  void safety(size_t s) {
    if (remaining() < s)
      buf.resize(2 * buf.size());
  }

  size_t remaining() const { return buf.size() - ofs; }

  vector<char> buf;
  size_t ofs;
};

void print_json2_runner(const JsonValue::JsonValuePtr &node, Buffer *buf) {

  switch (node->type()) {
    case JsonValue::JS_STRING:
      buf->add_string(node->to_string(), true);
      break;

    case JsonValue::JS_NUMBER:
      buf->add_number(node->to_number());
      break;

    case JsonValue::JS_INT:
      buf->add_int(node->to_int());
      break;

    case JsonValue::JS_BOOL:
      buf->add_string(node->to_bool() ? "true" : "false", false);
      break;

    case JsonValue::JS_ARRAY:
      buf->add_char('[');
      for (int i = 0; i < node->count(); ++i) {
        print_json2_runner(node->get(i), buf);
        if (i != node->count() - 1)
          buf->add_char(',');
      }
      buf->add_char(']');
      break;

    case JsonValue::JS_OBJECT: {
      buf->add_char('{');
      JsonObject *obj = (JsonObject *)node.get();
      int i = 0;
      for (auto it = begin(obj->kv()); it != end(obj->kv()); ++it) {
        buf->add_string(it->first, true);
        buf->add_char(':');
        print_json2_runner(it->second, buf);
        if (++i != obj->kv().size())
          buf->add_char(',');
      }
      buf->add_char('}');
      break;
    }
  }
}

std::string print_json2(const JsonValue::JsonValuePtr &root) {
  Buffer buf(10000);
  print_json2_runner(root, &buf);
  return string(buf.buf.data(), buf.ofs);
}

static void print_dispatch(const JsonValue *obj, int indent_level, std::string *res);
string print_json(const JsonValue::JsonValuePtr &root) {
  string res;
  print_dispatch(root.get(), 1, &res);
  return res;
}

static void print_inner(const JsonValue *obj, int indent_level, std::string *res) {

  switch (obj->type()) {
    case JsonValue::JS_STRING: 
      res->append("\"");
      res->append(obj->to_string());
      res->append("\"");
      break;

    case JsonValue::JS_NUMBER: {
      static char buf[2048];
      sprintf(buf, "%f", obj->to_number());
      res->append(buf);
      break;
    }

    case JsonValue::JS_INT:
      static char buf2[32];
      sprintf(buf2, "%d", obj->to_int());
      res->append(buf2);
      break;

    case JsonValue::JS_BOOL:
      res->append(obj->to_bool() ? "true" : "false");
      break;

    default:
      KASSERT(!"invalid type in print_inner");
      break;
  }
}

static void print_inner(const JsonObject *obj, int indent_level, std::string *res) {

  res->append("{\n");

  string indent_string(indent_level, '\t');

  auto it = begin(obj->_key_value);
  if (it != end(obj->_key_value)) {
    stringstream str;
    str << indent_string << "\"" << it->first << "\": ";
    res->append(str.str());
    print_dispatch(it->second.get(), indent_level + 1, res);
    ++it;
  }

  for (; it != end(obj->_key_value); ++it) {
    stringstream str;
    str << ",\n" << indent_string << "\"" << it->first << "\": ";
    res->append(str.str());

    print_dispatch(it->second.get(), indent_level + 1, res);
  }

  indent_string = string(indent_level - 1, '\t');

  stringstream str;
  str << "\n" << indent_string << "}";
  res->append(str.str());
}

static void print_inner(const JsonArray *obj, int indent_level, std::string *res) {
  string indent_string(indent_level, '\t');

  res->append("[\n");

  auto it = begin(obj->_value);
  if (it != end(obj->_value)) {
    res->append(indent_string);
    print_dispatch(it->get(), indent_level + 1, res);
    ++it;
  }

  for (; it != end(obj->_value); ++it) {
    stringstream str;
    str << ",\n" << indent_string;
    res->append(str.str());
    print_dispatch(it->get(), indent_level + 1, res);
  }

  indent_string = string(indent_level - 1, '\t');

  stringstream str;
  str << "\n" << indent_string << "]";
  res->append(str.str());
}

void print_dispatch(const JsonValue *obj, int indent_level, std::string *res) {
  // meh, this is kinda cheesy..
  switch (obj->type()) {
    case JsonValue::JS_ARRAY:
      return print_inner(static_cast<const JsonArray *>(obj), indent_level, res);
    case JsonValue::JS_OBJECT:
      return print_inner(static_cast<const JsonObject *>(obj), indent_level, res);
    default:
      return print_inner(obj, indent_level, res);
  }
}

//////////////////////////////////////////////////////////////////////////
// 
// parse json
//
//////////////////////////////////////////////////////////////////////////

static const char *skip_whitespace(const char *start, const char *end) {
  for (int i = 0; i < end - start; ++i) {
    if (!isspace((int)start[i]))
      return start + i;
  }
  return nullptr;
}

static const char *skip_delim(const char *start, const char *end, char delim) {
  while (start != end) {
    if (*start++ == delim) {
      while (*start == delim && start != end)
        start++;
      return start == end ? nullptr : start;
    }
  }
  return nullptr;
}

static bool between_delim(const char *start, const char *end, char delim, const char **key_start, const char **key_end) {
  int i;
  int len = end - start;
  for (i = 0; i < len; ++i) {
    if (start[i] == delim) {
      ++i;
      *key_start = start + i;
      for (; i < len; ++i) {
        if (start[i] == delim) {
          *key_end = start + i;
          return true;
        }
      }
    }
  }
  return false;
}

static bool is_json_digit(char ch) {
  return isdigit((int)ch) || ch == '-' || ch == '+' || ch == '.';
}

static JsonValue::JsonValuePtr parse_json_inner(const char *start, const char *end, const char **next) {
  const int len = end - start;
  if (len == 0)
    return JsonValue::emptyValue();
  const char *s = skip_whitespace(start, end);
  char ch = *s;

  if (ch == '{') {
    if (s = skip_whitespace(s + 1, end)) {
      auto a = JsonValue::create_object();
      while (true) {
        // parse the key
        const char *key_start, *key_end;
        if (between_delim(s, end, '"', &key_start, &key_end)) {
          string key(key_start, key_end - key_start);
          s = skip_delim(key_end, end, ':');
          a->add_key_value(key, parse_json_inner(s, end, &s));
          s = skip_whitespace(s, end);
          char ch = *s++;   // skip the } or ,
          if (ch == '}')
            break;
          if (ch != ',') {
            LOG_WARNING_LN("invalid json at: %s", s-1);
            KASSERT(false);
            break;
          }
        } else {
          // no key found (the file can still be valid)
          break;
        }
      }
      *next = s;
      return a;
    }

  } else if (ch == '[') {
    if (s = skip_whitespace(s + 1, end)) {
      auto a = JsonValue::create_array();
      while (true) {
        a->add_value(parse_json_inner(s, end, &s));
        s = skip_whitespace(s, end);
        char ch = *s++;   // skip the ] or ,
        if (ch == ']')
          break;
        if (ch != ',') {
          LOG_WARNING_LN("invalid json at: %s", s-1);
          KASSERT(false);
          break;
        }
      }
      *next = s;
      return a;
    }

  } else if (is_json_digit(ch)) {
    double v = atof(s);
    bool is_double = false;
    while (s != end && is_json_digit((int)*s)) {
      is_double |= *s == '.';
      s++;
    }
    *next = s;
    return is_double ? JsonValue::create_number(v) : JsonValue::create_int((int)v);

  } else if (strncmp(s, "true", min(4, len)) == 0) {
      *next = s + 4;
      return JsonValue::create_bool(true);

  } else if (strncmp(s, "false", min(5, len)) == 0) {
      *next = s + 5;
      return JsonValue::create_bool(false);

  } else if (ch == '\"') {
    const char *key_start, *key_end;
    if (between_delim(s, end, '"', &key_start, &key_end)) {
      string key(key_start, key_end - key_start);
      *next = key_end + 1;
      return JsonValue::create_string(key);
    }
  } else {
    KASSERT(!"UNKNOWN JSON VALUE");
  }

  return JsonValue::JsonValuePtr();
}

JsonValue::JsonValuePtr parse_json(const char *start, const char *end) {

  const char *tmp;
  return parse_json_inner(start, end, &tmp);
}
