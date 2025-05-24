// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright 2016-2025 Hristo Gochkov, Mathieu Carbou, Emil Muratov

#include "AsyncJson.h"

#if ASYNC_JSON_SUPPORT == 1

#if ARDUINOJSON_VERSION_MAJOR == 5
AsyncJsonResponse::AsyncJsonResponse(bool isArray) : _isValid{false} {
  _code = 200;
  _contentType = asyncsrv::T_application_json;
  if (isArray) {
    _root = _jsonBuffer.createArray();
  } else {
    _root = _jsonBuffer.createObject();
  }
}
#elif ARDUINOJSON_VERSION_MAJOR == 6
AsyncJsonResponse::AsyncJsonResponse(bool isArray, size_t maxJsonBufferSize) : _jsonBuffer(maxJsonBufferSize), _isValid{false} {
  _code = 200;
  _contentType = asyncsrv::T_application_json;
  if (isArray) {
    _root = _jsonBuffer.createNestedArray();
  } else {
    _root = _jsonBuffer.createNestedObject();
  }
}
#else
AsyncJsonResponse::AsyncJsonResponse(bool isArray) : _isValid{false} {
  _code = 200;
  _contentType = asyncsrv::T_application_json;
  if (isArray) {
    _root = _jsonBuffer.add<JsonArray>();
  } else {
    _root = _jsonBuffer.add<JsonObject>();
  }
}
#endif

size_t AsyncJsonResponse::setLength() {
#if ARDUINOJSON_VERSION_MAJOR == 5
  _contentLength = _root.measureLength();
#else
  _contentLength = measureJson(_root);
#endif
  if (_contentLength) {
    _isValid = true;
  }
  return _contentLength;
}

size_t AsyncJsonResponse::_fillBuffer(uint8_t *data, size_t len) {
  ChunkPrint dest(data, _sentLength, len);
#if ARDUINOJSON_VERSION_MAJOR == 5
  _root.printTo(dest);
#else
  serializeJson(_root, dest);
#endif
  return len;
}

#if ARDUINOJSON_VERSION_MAJOR == 6
PrettyAsyncJsonResponse::PrettyAsyncJsonResponse(bool isArray, size_t maxJsonBufferSize) : AsyncJsonResponse{isArray, maxJsonBufferSize} {}
#else
PrettyAsyncJsonResponse::PrettyAsyncJsonResponse(bool isArray) : AsyncJsonResponse{isArray} {}
#endif

size_t PrettyAsyncJsonResponse::setLength() {
#if ARDUINOJSON_VERSION_MAJOR == 5
  _contentLength = _root.measurePrettyLength();
#else
  _contentLength = measureJsonPretty(_root);
#endif
  if (_contentLength) {
    _isValid = true;
  }
  return _contentLength;
}

size_t PrettyAsyncJsonResponse::_fillBuffer(uint8_t *data, size_t len) {
  ChunkPrint dest(data, _sentLength, len);
#if ARDUINOJSON_VERSION_MAJOR == 5
  _root.prettyPrintTo(dest);
#else
  serializeJsonPretty(_root, dest);
#endif
  return len;
}

#if ARDUINOJSON_VERSION_MAJOR == 6
AsyncCallbackJsonWebHandler::AsyncCallbackJsonWebHandler(const String &uri, ArJsonRequestHandlerFunction onRequest, size_t maxJsonBufferSize)
  : _uri(uri), _method(HTTP_GET | HTTP_POST | HTTP_PUT | HTTP_PATCH), _onRequest(onRequest), maxJsonBufferSize(maxJsonBufferSize), _maxContentLength(16384) {}
#else
AsyncCallbackJsonWebHandler::AsyncCallbackJsonWebHandler(const String &uri, ArJsonRequestHandlerFunction onRequest)
  : _uri(uri), _method(HTTP_GET | HTTP_POST | HTTP_PUT | HTTP_PATCH), _onRequest(onRequest), _maxContentLength(16384) {}
#endif

bool AsyncCallbackJsonWebHandler::canHandle(AsyncWebServerRequest *request) const {
  if (!_onRequest || !request->isHTTP() || !(_method & request->method())) {
    return false;
  }

  if (_uri.length() && (_uri != request->url() && !request->url().startsWith(_uri + "/"))) {
    return false;
  }

  if (request->method() != HTTP_GET && !request->contentType().equalsIgnoreCase(asyncsrv::T_application_json)) {
    return false;
  }

  return true;
}

void AsyncCallbackJsonWebHandler::handleRequest(AsyncWebServerRequest *request) {
  if (_onRequest) {
    if (request->method() == HTTP_GET) {
      JsonVariant json;
      _onRequest(request, json);
      return;
    }
    // this is not a GET
    // check if body is too large, if it is, don't parse
    if (request->contentLength() > _maxContentLength) {
      request->send(413);
      return;
    }

    // try to parse body as JSON
    if (request->_tempObject != NULL) {
      size_t dataSize =
        min(request->contentLength(), request->_tempSize);  // smaller value of contentLength or the size of the buffer. normally those should match.
#if ARDUINOJSON_VERSION_MAJOR == 5
      DynamicJsonBuffer jsonBuffer;
      uint8_t *p = (uint8_t *)(request->_tempObject);
      p[dataSize] = '\0';  // null terminate, assume we allocated one extra char
      // parse can only get null terminated strings as parameters
      JsonVariant json = jsonBuffer.parse(p);
      if (json.success()) {
#elif ARDUINOJSON_VERSION_MAJOR == 6
      DynamicJsonDocument jsonBuffer(this->maxJsonBufferSize);
      DeserializationError error = deserializeJson(jsonBuffer, (uint8_t *)(request->_tempObject), dataSize);
      if (!error) {
        JsonVariant json = jsonBuffer.as<JsonVariant>();
#else
      JsonDocument jsonBuffer;
      // deserializeJson expects a null terminated string or a pointer plus length
      DeserializationError error = deserializeJson(jsonBuffer, (uint8_t *)(request->_tempObject), dataSize);
      if (!error) {
        JsonVariant json = jsonBuffer.as<JsonVariant>();
#endif

        _onRequest(request, json);
        return;
      }
    }
    // there is no body, no buffer or we had an error parsing the body
    request->send(400);
  } else {  // if no _onRequest
    request->send(500);
  }
}

void AsyncCallbackJsonWebHandler::handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
  if (_onRequest) {
    if (total > 0 && request->_tempObject == NULL && total < _maxContentLength) {  // if request content length is valid size and we have no content buffer yet
      request->_tempObject = malloc(total + 1);  // allocate one additional byte so we can null terminate this buffer (needed for ArduinoJson 5)
      if (request->_tempObject == NULL) {        // if allocation failed
#ifdef ESP32
        log_e("Failed to allocate");
#endif
        request->abort();
        return;
      }
      request->_tempSize = total;  // store the size of allocation we made into _tempSize
    }
    if (request->_tempObject != NULL) {
      // check if the buffer is the right size so we don't write out of bounds
      if (request->_tempSize >= total) {
        memcpy((uint8_t *)(request->_tempObject) + index, data, len);
      } else {
#ifdef ESP32
        log_e("Bad size of temp buffer");
#endif
        request->abort();
        return;
      }
    }
  }
}

#endif  // ASYNC_JSON_SUPPORT
