
#include <v8.h>
#include <libxml/HTMLparser.h>
#include <libxml/xpathInternals.h>
#include <libxslt/xslt.h>
#include <libxslt/xsltInternals.h>
#include <libxslt/transform.h>
#include <libxslt/xsltutils.h>
#include <string.h>
#include <stdio.h>
#include "macros.h"
#include "scopeguard.h"
#include <libexslt/exslt.h>

#define OBJ_DESTRUCTOR(d) Persistent<Object> _weak_handle = Persistent<Object>::New(self); \
                          _weak_handle.MakeWeak(NULL, d);

using namespace v8;

void jsXmlDocCleanup(Persistent<Value> value, void *) {
    HandleScope handlescope;
    Local<Object> obj = value->ToObject();
    EXTERNAL(xmlDocPtr, doc, obj, 0);
    xmlFreeDoc(doc);
    return;
}

void jsXsltStylesheetCleanup(Persistent<Value> value, void *) {
    HandleScope handlescope;
    Local<Object> obj = value->ToObject();
    EXTERNAL(xsltStylesheetPtr, style, obj, 0);
    xsltFreeStylesheet(style);
    return;
}

OBJECT(jsXmlDoc, 1, xmlDocPtr doc)
    INTERNAL(0, doc)
    OBJ_DESTRUCTOR(&jsXmlDocCleanup)
    RETURN_SCOPED(self);
END

OBJECT(jsXsltStylesheet, 1, xsltStylesheetPtr style)
    INTERNAL(0, style)
    OBJ_DESTRUCTOR(&jsXsltStylesheetCleanup)
    RETURN_SCOPED(self);
END

FUNCTION(readXmlString)
    ARG_COUNT(1)
    ARG_utf8(str, 0)

    xmlDocPtr doc = xmlReadMemory(*str, str.length(), NULL, "UTF-8", 0);
    if (!doc) {
        return JS_ERROR("Failed to parse XML");
    }
    RETURN_SCOPED(jsXmlDoc(doc));
END

FUNCTION(readHtmlString)
    ARG_COUNT(1)
    ARG_utf8(str, 0)

    htmlDocPtr doc = htmlReadMemory(*str, str.length(), NULL, "UTF-8", HTML_PARSE_RECOVER);
    if (!doc) {
        return JS_ERROR("Failed to parse HTML");
    }
    RETURN_SCOPED(jsXmlDoc(doc));
END

FUNCTION(readXsltString)
    ARG_COUNT(1)
    ARG_utf8(str, 0)

    xmlDocPtr doc = xmlReadMemory(*str, str.length(), NULL, "UTF-8", 0);
    if (!doc) {
        return JS_ERROR("Failed to parse XML");
    }
    ScopeGuard guard =  MakeGuard(xmlFreeDoc, doc);

    xsltStylesheetPtr stylesheet = xsltParseStylesheetDoc(doc);
    if (!stylesheet) {
        return JS_ERROR("Failed to parse stylesheet");
    }
    guard.Dismiss();
    RETURN_SCOPED(jsXsltStylesheet(stylesheet));
END

void freeArray(char **array, int size) {
    for (int i = 0; i < size; i++) {
        free(array[i]);
    }
    free(array);
}

FUNCTION(xpathEval)
    ARG_COUNT(3)
    ARG_obj(objDocument, 0)
    ARG_utf8(xpathExpr, 1)
    ARG_array(nsArray, 2)

    EXTERNAL(xmlDocPtr, document, objDocument, 0);

    uint32_t nsArrayLen = nsArray->Length();
    if (nsArrayLen % 2 != 0) {
        return JS_ERROR("Namespace array contains an odd number of parameters");
    }
    char** namespaces = (char **)malloc(sizeof(char *) * nsArrayLen);

    if (!namespaces) {
        return JS_ERROR("Failed to allocate memory");
    }
    memset(namespaces, 0, sizeof(char *) * nsArrayLen);
    ON_BLOCK_EXIT(freeArray, namespaces, nsArrayLen);

    for (int i = 0; i < nsArrayLen; i++) {
        Local<String> ns = nsArray->Get(JS_int(i))->ToString();
        namespaces[i] = (char *)malloc(sizeof(char) * (ns->Length() + 1));
        if (!namespaces[i]) {
            return JS_ERROR("Failed to allocate memory");
        }
        ns->WriteAscii(namespaces[i]);
    }

    xmlXPathContextPtr xpathCtxt = xmlXPathNewContext(document);
    if (xpathCtxt == NULL) {
        return JS_ERROR("Failed to create new xpath context");
    }
    for (int i = 0; i < nsArrayLen; i+=2) {
        if(xmlXPathRegisterNs(xpathCtxt, (xmlChar*)namespaces[i], (xmlChar*)namespaces[i+1]) != 0) {
            return JS_ERROR("Failed to register config namespace");
        }
    }
    xmlXPathObjectPtr ret = xmlXPathEvalExpression((xmlChar *)*xpathExpr, xpathCtxt);
    if (!ret) {
        return JS_ERROR("Bad xpath");
    }
    xmlXPathFreeContext(xpathCtxt);

    //FIXME: Do we really have to return something??? What makes sense?
    RETURN_SCOPED(JS_str2("true",strlen("true")));
END

FUNCTION(transform)
    ARG_COUNT(4)
    ARG_obj(objStylesheet, 0)
    ARG_obj(objDocument, 1)
    ARG_array(paramArray, 2)
    ARG_array(stringParamArray, 3)

    EXTERNAL(xsltStylesheetPtr, stylesheet, objStylesheet, 0);
    EXTERNAL(xmlDocPtr, document, objDocument, 0);

    uint32_t paramArrayLen = paramArray->Length();
    if (paramArrayLen % 2 != 0) {
        return JS_ERROR("Param array contains an odd number of parameters");
    }

    uint32_t stringParamArrayLen = stringParamArray->Length();
    if (stringParamArrayLen % 2 != 0) {
        return JS_ERROR("StringParam array contains an odd number of parameters");
    }

    char** params = (char **)malloc(sizeof(char *) * (paramArrayLen + stringParamArrayLen + 1));

    if (!params) {
        return JS_ERROR("Failed to allocate memory");
    }
    memset(params, 0, sizeof(char *) * (paramArrayLen + stringParamArrayLen + 1));
    ON_BLOCK_EXIT(freeArray, params, (paramArrayLen + stringParamArrayLen + 1));

    int offset = 0;

    for (int i = 0; i < paramArray->Length(); i++) {
        Local<String> param = paramArray->Get(JS_int(i))->ToString();
        params[i] = (char *)malloc(sizeof(char) * (param->Length() + 1));
        if (!params[i]) {
            return JS_ERROR("Failed to allocate memory");
        }
        param->WriteAscii(params[i]);
        offset = i;
    }

    for (int i = 0; i < stringParamArray->Length(); i++) {
        Local<String> stringParam = stringParamArray->Get(JS_int(i))->ToString();
        params[i+offset] = (char *)malloc(sizeof(char) * (stringParam->Length() + 2 + 1));// +2 for leading and trailing " chars
        if (!params[i+offset]) {
            return JS_ERROR("Failed to allocate memory");
        }
        if ((i % 2) == 1) {
            params[i+offset][0] = '"';
            stringParam->WriteAscii(&params[i+offset][1]);
            params[i+offset][stringParam->Length() + 1] = '"';
            params[i+offset][stringParam->Length() + 1 + 1] = '\0';
        }else{
            stringParam->WriteAscii(params[i+offset]);
        }
    }

    try {
        xmlDocPtr result = xsltApplyStylesheet(stylesheet, document, (const char **)params);
        if (!result) {
            throw JS_ERROR("Failed to apply stylesheet");
        }
        ON_BLOCK_EXIT(xmlFreeDoc, result);

        xmlChar *doc_ptr;
        int doc_len;
        xsltSaveResultToString(&doc_ptr, &doc_len, result, stylesheet);
        ON_BLOCK_EXIT(xmlFree, doc_ptr);

        RETURN_SCOPED(JS_str2((const char *)doc_ptr, doc_len));
    } catch (Handle<Value> err) {
        return err;
    }
END

extern "C" void init(Handle<Object> target)
{
    HandleScope scope;
	
    exsltRegisterAll();
    Handle<Object> self = target;
    BIND("readXmlString", readXmlString);
    BIND("readHtmlString", readHtmlString);
    BIND("readXsltString", readXsltString);
    BIND("transform", transform);
    BIND("xpathEval", xpathEval);
}
