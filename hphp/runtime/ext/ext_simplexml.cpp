/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-2013 Facebook, Inc. (http://www.facebook.com)     |
   | Copyright (c) 1997-2010 The PHP Group                                |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#include "hphp/runtime/ext/ext_simplexml.h"
#include "hphp/runtime/ext/ext_file.h"
#include "hphp/runtime/ext/ext_class.h"
#include "hphp/runtime/ext/ext_domdocument.h"
#include "hphp/runtime/base/class-info.h"
#include "hphp/runtime/base/request-local.h"
#include "hphp/system/systemlib.h"

namespace HPHP {

IMPLEMENT_DEFAULT_EXTENSION(SimpleXML);

///////////////////////////////////////////////////////////////////////////////
// Helpers

#define SKIP_TEXT(__p) \
  if ((__p)->type == XML_TEXT_NODE) { \
    goto next_iter; \
  }

static c_SimpleXMLElement *_node_as_zval(c_SimpleXMLElement *sxe, xmlNodePtr node, 
                                         SXE_ITER itertype, char *name, 
                                         const xmlChar *nsprefix, int isprefix) {
  Object obj = create_object(sxe->o_getClassName(), Array(), false);
  c_SimpleXMLElement *subnode = obj.getTyped<c_SimpleXMLElement>();
  subnode->document = sxe->document;
  subnode->iter.type = itertype;
  if (name) {
    subnode->iter.name = xmlStrdup((xmlChar *)name);
  }
  if (nsprefix && *nsprefix) {
    subnode->iter.nsprefix = xmlStrdup(nsprefix);
    subnode->iter.isprefix = isprefix;
  }
  subnode->node = node; 
  return subnode;
}

static inline int match_ns(c_SimpleXMLElement *sxe, xmlNodePtr node, xmlChar *name, 
                           int prefix) {
  if (name == nullptr && (node->ns == nullptr || node->ns->prefix == nullptr)) {
    return 1;
  }

  if (node->ns && !xmlStrcmp(prefix ? node->ns->prefix : node->ns->href, name)) {
    return 1;
  }

  return 0;
}


static xmlNodePtr php_sxe_iterator_fetch(c_SimpleXMLElement *sxe, xmlNodePtr node, int use_data) {
  xmlChar *prefix  = sxe->iter.nsprefix;
  int isprefix  = sxe->iter.isprefix;
  int test_elem = sxe->iter.type == SXE_ITER_ELEMENT  && sxe->iter.name;
  int test_attr = sxe->iter.type == SXE_ITER_ATTRLIST && sxe->iter.name;

  while (node) {
    SKIP_TEXT(node);
    if (sxe->iter.type != SXE_ITER_ATTRLIST && node->type == XML_ELEMENT_NODE) {
      if ((!test_elem || !xmlStrcmp(node->name, sxe->iter.name)) && match_ns(sxe, node, prefix, isprefix)) {
        break;
      }
    } else if (node->type == XML_ATTRIBUTE_NODE) {
      if ((!test_attr || !xmlStrcmp(node->name, sxe->iter.name)) && match_ns(sxe, node, prefix, isprefix)) {
        break;
      }
    }
next_iter:
    node = node->next;
  }

  if (node && use_data) {
    sxe->iter.data = _node_as_zval(sxe, node, SXE_ITER_NONE, nullptr, prefix, isprefix);
  }

  return node;
}

static void php_sxe_move_forward_iterator(c_SimpleXMLElement *sxe) {
  xmlNodePtr node = nullptr;
  if (sxe->iter.data) {
    node = sxe->iter.data->node;
    sxe->iter.data = nullptr;
  }

  if (node) {
    php_sxe_iterator_fetch(sxe, node->next, 1);
  }
}

static xmlNodePtr php_sxe_reset_iterator(c_SimpleXMLElement *sxe, int use_data) {
  if (sxe->iter.data) {
    sxe->iter.data = nullptr;
  }
        
  xmlNodePtr node = sxe->node;
  if (node) {
    switch (sxe->iter.type) {
      case SXE_ITER_ELEMENT:
      case SXE_ITER_CHILD:
      case SXE_ITER_NONE:
        node = node->children;
        break;
      case SXE_ITER_ATTRLIST:
        node = (xmlNodePtr) node->properties;
    }
    return php_sxe_iterator_fetch(sxe, node, use_data);
  }
  return nullptr;
}

static xmlNodePtr php_sxe_get_first_node(c_SimpleXMLElement *sxe) {
  if (sxe && sxe->iter.type != SXE_ITER_NONE) {
    php_sxe_reset_iterator(sxe, 1);
    xmlNodePtr ret = nullptr;
    if (sxe->iter.data) {
      ret = sxe->iter.data->node; 
    }
    return ret;
  } else {
    return sxe->node;
  }
}

static Variant cast_object(char *contents, int type) {
  String str = String((char*)contents);
  Variant obj;
  switch (type) {
    case HPHP::KindOfString:
      obj = str;
      break;
    case HPHP::KindOfInt64:
      obj = toInt64(str); 
      break;
    case HPHP::KindOfDouble:
      obj = toDouble(str);
      break;
  }
  return obj;
}

static Variant sxe_object_cast(c_SimpleXMLElement *sxe, int type) {
  if (type == HPHP::KindOfBoolean) {
    xmlNodePtr node = php_sxe_get_first_node(sxe);
    return node != nullptr || sxe->hasDynProps();
  }

  xmlChar *contents = nullptr;
  if (sxe->iter.type != SXE_ITER_NONE) {
    xmlNodePtr node = php_sxe_get_first_node(sxe);
    if (node) {
      contents = xmlNodeListGetString(sxe->document, node->children, 1);
    }
  } else {
    if (!sxe->node) {
       if (sxe->document) {
         sxe->node = xmlDocGetRootElement(sxe->document);
       }
    }

    if (sxe->node) {
      if (sxe->node->children) {
        contents = xmlNodeListGetString(sxe->document, sxe->node->children, 1);
      }
    } 
  }

  Variant obj = cast_object((char*)contents, type);

  if (contents) {
    xmlFree(contents);
  }
  return obj;
}

///////////////////////////////////////////////////////////////////////////////
// SimpleXML 

Variant f_simplexml_import_dom(CObjRef node,
                               const String& class_name /* = "SimpleXMLElement" */) {
  return false;
}

Variant f_simplexml_load_string(const String& data,
                                const String& class_name /* = "SimpleXMLElement" */,
                                int64_t options /* = 0 */,
                                const String& ns /* = "" */,
                                bool is_prefix /* = false */) {
  xmlDocPtr document = xmlReadMemory(data.data(), data.size(), nullptr, nullptr, options);
  if (!document) {
    return false;
  }

  Class* cls;
  if (!class_name.empty()) {
    cls = Unit::loadClass(class_name.get());
    if (!cls) {
      throw_invalid_argument("class not found: %s", class_name.data());
      return uninit_null();
    }
    if (!cls->classof(c_SimpleXMLElement::classof())) {
      throw_invalid_argument(
        "simplexml_load_string() expects parameter 2 to be a class name "
        "derived from SimpleXMLElement, '%s' given",
        class_name.data());
      return uninit_null();
    }
  } else {
    cls = c_SimpleXMLElement::classof();
  }

  Object obj = create_object(cls->nameRef(), Array(), false);
  c_SimpleXMLElement *sxe = obj.getTyped<c_SimpleXMLElement>();
  sxe->document = document;
  sxe->node = xmlDocGetRootElement(document);
  sxe->iter.nsprefix = ns.size() ? xmlStrdup((xmlChar *)ns.data()) : nullptr;
  sxe->iter.isprefix = is_prefix;
  return obj;
}

Variant f_simplexml_load_file(const String& filename,
                              const String& class_name /* = "SimpleXMLElement" */,
                              int64_t options /* = 0 */, const String& ns /* = "" */,
                              bool is_prefix /* = false */) {
  String str = f_file_get_contents(filename);
  return f_simplexml_load_string(str, class_name, options, ns, is_prefix);
}

///////////////////////////////////////////////////////////////////////////////
// SimpleXMLElement

void c_SimpleXMLElement::sweep() {
}

c_SimpleXMLElement::c_SimpleXMLElement(Class* cb) :
    ExtObjectDataFlags<ObjectData::UseGet|
                       ObjectData::UseSet|
                       ObjectData::UseIsset|
                       ObjectData::UseUnset|
                       ObjectData::CallToImpl|
                       ObjectData::HasClone>(cb),
      document(nullptr), node(nullptr) {
}

c_SimpleXMLElement::~c_SimpleXMLElement() {
  if (iter.data) {
    iter.data = nullptr;
  }

  if (iter.name) {
    xmlFree(iter.name);
    iter.name = nullptr;
  }
  if (iter.nsprefix) {
    xmlFree(iter.nsprefix);
    iter.nsprefix = nullptr;
  }
}

void c_SimpleXMLElement::t___construct(const String& data, int64_t options /* = 0 */,
                                       bool data_is_url /* = false */,
                                       const String& ns /* = "" */,
                                       bool is_prefix /* = false */) {
}

Variant c_SimpleXMLElement::t_xpath(const String& path) {
  return false;
}

bool c_SimpleXMLElement::t_registerxpathnamespace(const String& prefix, const String& ns) {
  return false;
}

Variant c_SimpleXMLElement::t_asxml(const String& filename /* = "" */) {
  return false;
}

Array c_SimpleXMLElement::t_getnamespaces(bool recursive /* = false */) {
  return Array::Create(); 
}

Array c_SimpleXMLElement::t_getdocnamespaces(bool recursive /* = false */) {
  return Array::Create();
}

Object c_SimpleXMLElement::t_children(const String& ns /* = "" */,
                                      bool is_prefix /* = false */) {
  return nullptr;
}

String c_SimpleXMLElement::t_getname() {
  xmlNodePtr node = php_sxe_get_first_node(this);
  if (node) {
    return String((char*)node->name);
  }
  return ""; 
}

Object c_SimpleXMLElement::t_attributes(const String& ns /* = "" */,
                                        bool is_prefix /* = false */) {
  return nullptr;
}

Variant c_SimpleXMLElement::t_addchild(const String& qname,
                                       const String& value /* = null_string */,
                                       const String& ns /* = null_string */) {
  return false;
}

void c_SimpleXMLElement::t_addattribute(const String& qname,
                                        const String& value /* = null_string */,
                                        const String& ns /* = null_string */) {
}

String c_SimpleXMLElement::t___tostring() {
  return sxe_object_cast(this, HPHP::KindOfString);
}

Variant c_SimpleXMLElement::t___get(Variant name) {
  return false;
}

Variant c_SimpleXMLElement::t___unset(Variant name) {
  return false;
}

bool c_SimpleXMLElement::t___isset(Variant name) {
  return false;
}

Variant c_SimpleXMLElement::t___set(Variant name, Variant value) {
  return false;
}

c_SimpleXMLElement* c_SimpleXMLElement::Clone(ObjectData* obj) {
  auto sxe = static_cast<c_SimpleXMLElement*>(obj);
  c_SimpleXMLElement *clone = static_cast<c_SimpleXMLElement*>(obj->cloneImpl());
  clone->document = sxe->document;

  clone->iter.isprefix = sxe->iter.isprefix;
  if (sxe->iter.name != nullptr) {
    clone->iter.name = xmlStrdup((xmlChar *)sxe->iter.name);
  }
  if (sxe->iter.nsprefix != nullptr) {
    clone->iter.nsprefix = xmlStrdup((xmlChar *)sxe->iter.nsprefix);
  }
  clone->iter.type = sxe->iter.type;

  if (sxe->node) {
    clone->node = xmlDocCopyNode(sxe->node, sxe->document, 1);
  }

  return clone;
}

bool c_SimpleXMLElement::ToBool(const ObjectData* obj) noexcept {
  return false;
}

int64_t c_SimpleXMLElement::ToInt64(const ObjectData* obj) noexcept {
  return 0;
}

double c_SimpleXMLElement::ToDouble(const ObjectData* obj) noexcept {
  return false;
}

Array c_SimpleXMLElement::ToArray(const ObjectData* obj) {
  return Array::Create(); 
}

Variant c_SimpleXMLElement::t_getiterator() {
  php_sxe_reset_iterator(this, 1);
  iter.sxe = this;
  return Object(&iter);
}

int64_t c_SimpleXMLElement::t_count() {
  return 0;
}

///////////////////////////////////////////////////////////////////////////////
// ArrayAccess

bool c_SimpleXMLElement::t_offsetexists(CVarRef index) {
  return false;
}

Variant c_SimpleXMLElement::t_offsetget(CVarRef index) {
  return false;
}

void c_SimpleXMLElement::t_offsetset(CVarRef index, CVarRef newvalue) {
}

void c_SimpleXMLElement::t_offsetunset(CVarRef index) {
}

///////////////////////////////////////////////////////////////////////////////
// Iterator

c_SimpleXMLElementIterator::c_SimpleXMLElementIterator(Class* cb) :
    ExtObjectData(cb), name(nullptr), nsprefix(nullptr), isprefix(false),
    type(SXE_ITER_NONE) {
}

c_SimpleXMLElementIterator::~c_SimpleXMLElementIterator() {
}

void c_SimpleXMLElementIterator::sweep() {
}

void c_SimpleXMLElementIterator::t___construct() {
}

Variant c_SimpleXMLElementIterator::t_current() {
  // TODO:
  // return Object(data);
  return false;
}

Variant c_SimpleXMLElementIterator::t_key() {
  return false;
}

Variant c_SimpleXMLElementIterator::t_next() {
  php_sxe_move_forward_iterator(sxe);
  return uninit_null();
}

Variant c_SimpleXMLElementIterator::t_rewind() {
  php_sxe_reset_iterator(sxe);
  return uninit_null();
}

Variant c_SimpleXMLElementIterator::t_valid() {
  return (bool)sxe->iter.data;
}

///////////////////////////////////////////////////////////////////////////////
// LibXMLError

c_LibXMLError::c_LibXMLError(Class* cb) :
    ExtObjectData(cb) {
}

c_LibXMLError::~c_LibXMLError() {
}

void c_LibXMLError::t___construct() {
}

///////////////////////////////////////////////////////////////////////////////
// libxml

class xmlErrorVec : public std::vector<xmlError> {
public:
  ~xmlErrorVec() {
    reset();
  }

  void reset() {
    for (unsigned int i = 0; i < size(); i++) {
      xmlResetError(&at(i));
    }
    clear();
  }
};

class LibXmlErrors : public RequestEventHandler {
public:
  virtual void requestInit() {
    m_use_error = false;
    m_errors.reset();
    xmlParserInputBufferCreateFilenameDefault(nullptr);
  }
  virtual void requestShutdown() {
    m_use_error = false;
    m_errors.reset();
  }

  bool m_use_error;
  xmlErrorVec m_errors;
};

IMPLEMENT_STATIC_REQUEST_LOCAL(LibXmlErrors, s_libxml_errors);

bool libxml_use_internal_error() {
  return s_libxml_errors->m_use_error;
}

extern void libxml_add_error(const std::string &msg) {
  xmlErrorVec *error_list = &s_libxml_errors->m_errors;

  error_list->resize(error_list->size() + 1);
  xmlError &error_copy = error_list->back();
  memset(&error_copy, 0, sizeof(xmlError));

  error_copy.domain = 0;
  error_copy.code = XML_ERR_INTERNAL_ERROR;
  error_copy.level = XML_ERR_ERROR;
  error_copy.line = 0;
  error_copy.node = nullptr;
  error_copy.int1 = 0;
  error_copy.int2 = 0;
  error_copy.ctxt = nullptr;
  error_copy.message = (char*)xmlStrdup((const xmlChar*)msg.c_str());
  error_copy.file = nullptr;
  error_copy.str1 = nullptr;
  error_copy.str2 = nullptr;
  error_copy.str3 = nullptr;
}

static void libxml_error_handler(void *userData, xmlErrorPtr error) {
  xmlErrorVec *error_list = &s_libxml_errors->m_errors;

  error_list->resize(error_list->size() + 1);
  xmlError &error_copy = error_list->back();
  memset(&error_copy, 0, sizeof(xmlError));

  if (error) {
    xmlCopyError(error, &error_copy);
  } else {
    error_copy.code = XML_ERR_INTERNAL_ERROR;
    error_copy.level = XML_ERR_ERROR;
  }
}

const StaticString
  s_level("level"),
  s_code("code"),
  s_column("column"),
  s_message("message"),
  s_file("file"),
  s_line("line");

static Object create_libxmlerror(xmlError &error) {
  Object ret(NEWOBJ(c_LibXMLError)());
  ret->o_set(s_level,   error.level);
  ret->o_set(s_code,    error.code);
  ret->o_set(s_column,  error.int2);
  ret->o_set(s_message, String(error.message, CopyString));
  ret->o_set(s_file,    String(error.file, CopyString));
  ret->o_set(s_line,    error.line);
  return ret;
}

Variant f_libxml_get_errors() {
  xmlErrorVec *error_list = &s_libxml_errors->m_errors;
  Array ret = Array::Create();
  for (unsigned int i = 0; i < error_list->size(); i++) {
    ret.append(create_libxmlerror(error_list->at(i)));
  }
  return ret;
}

Variant f_libxml_get_last_error() {
  xmlErrorPtr error = xmlGetLastError();
  if (error) {
    return create_libxmlerror(*error);
  }
  return false;
}

void f_libxml_clear_errors() {
  xmlResetLastError();
  s_libxml_errors->m_errors.reset();
}

bool f_libxml_use_internal_errors(CVarRef use_errors /* = null_variant */) {
  bool ret = (xmlStructuredError == libxml_error_handler);
  if (!use_errors.isNull()) {
    if (!use_errors.toBoolean()) {
      xmlSetStructuredErrorFunc(nullptr, nullptr);
      s_libxml_errors->m_use_error = false;
      s_libxml_errors->m_errors.reset();
    } else {
      xmlSetStructuredErrorFunc(nullptr, libxml_error_handler);
      s_libxml_errors->m_use_error = true;
    }
  }
  return ret;
}

void f_libxml_set_streams_context(CResRef streams_context) {
  throw NotImplementedException(__func__);
}

static xmlParserInputBufferPtr
hphp_libxml_input_buffer_noload(const char *URI, xmlCharEncoding enc) {
  return nullptr;
}

bool f_libxml_disable_entity_loader(bool disable /* = true */) {
  xmlParserInputBufferCreateFilenameFunc old;

  if (disable) {
    old = xmlParserInputBufferCreateFilenameDefault(hphp_libxml_input_buffer_noload);
  } else {
    old = xmlParserInputBufferCreateFilenameDefault(nullptr);
  }
  return (old == hphp_libxml_input_buffer_noload);
}

///////////////////////////////////////////////////////////////////////////////
}
