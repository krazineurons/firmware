/*
 * Copyright (c) 2016 Particle Industries, Inc.  All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE // for strchrnul()
#endif
#include <cstring>

#include "spark_wiring_logging.h"

#include <algorithm>
#include <cinttypes>
#include <memory>

#include "spark_wiring_usbserial.h"
#include "spark_wiring_usartserial.h"

namespace {

using namespace spark;

/*
    This class performs processing of configuration requests in JSON format.

    Adding log handler:

    {
      "cmd": "addHandler", // Command name
      "id": "handler1", // Handler ID
      "hnd": { // Handler settings
        "type": "JSONLogHandler", // Handler type
        "param": { // Additional handler parameters
          ...
        }
      },
      "strm": { // Stream settings
        "type": "Serial1", // Stream type
        "param": { // Additional stream parameters
          ...
        }
      }
      "filt": [ // Category filters
        {
          "cat": "app", // Category name
          "lvl": "all" // Logging level
        }
      ],
      "lvl": "warn" // Default logging level
    }

    Removing log handler:

    {
      "cmd": "removeHandler", // Command name
      "id": "handler1" // Handler ID
    }

    Enumerating active log handlers:

    {
      "cmd": "enumHandlers" // Command name
    }

    Reply example:

    [
      "handler1", "handler2"
    ]
*/
class JSONRequestHandler {
public:
    static bool process(char *buf, size_t bufSize, size_t reqSize, size_t *repSize) {
        const JSONValue jsonReq = JSONValue::parse(buf, reqSize);
        if (!jsonReq.isValid()) {
            return false; // Parsing error
        }
        Request req;
        if (!parseRequest(jsonReq, &req)) {
            return false;
        }
        JSONBufferWriter writer(buf, bufSize);
        if (!processRequest(req, writer)) {
            return false;
        }
        if (repSize) {
            *repSize = writer.dataSize();
        }
        return true;
    }

private:
    struct Object {
        JSONString type;
        JSONValue params;
    };

    struct Request {
        Object handler, stream;
        LogCategoryFilters filters;
        JSONString cmd, id;
        LogLevel level;

        Request() :
                level(LOG_LEVEL_NONE) {
        }
    };

    static bool parseRequest(const JSONValue &value, Request *req) {
        JSONObjectIterator it(value);
        while (it.next()) {
            if (it.name() == "cmd") { // Command name
                req->cmd = it.value().toString();
            } else if (it.name() == "id") { // Handler ID
                req->id = it.value().toString();
            } else if (it.name() == "hnd") { // Handler settings
                if (!parseObject(it.value(), &req->handler)) {
                    return false;
                }
            } else if (it.name() == "strm") { // Stream settings
                if (!parseObject(it.value(), &req->stream)) {
                    return false;
                }
            } else if (it.name() == "filt") { // Category filters
                if (!parseFilters(it.value(), &req->filters)) {
                    return false;
                }
            } else if (it.name() == "lvl") { // Default level
                if (!parseLevel(it.value(), &req->level)) {
                    return false;
                }
            }
        }
        return true;
    }

    static bool parseObject(const JSONValue &value, Object *object) {
        JSONObjectIterator it(value);
        while (it.next()) {
            if (it.name() == "type") { // Object type
                object->type = it.value().toString();
            } else if (it.name() == "params") { // Additional parameters
                object->params = it.value();
            }
        }
        return true;
    }

    static bool parseFilters(const JSONValue &value, LogCategoryFilters *filters) {
        JSONArrayIterator it(value);
        if (!filters->reserve(it.count())) {
            return false; // Memory allocation error
        }
        while (it.next()) {
            JSONString cat;
            LogLevel level = LOG_LEVEL_NONE;
            JSONObjectIterator it2(it.value());
            while (it2.next()) {
                if (it2.name() == "cat") { // Category
                    cat = it2.value().toString();
                } else if (it2.name() == "lvl") { // Level
                    if (!parseLevel(it2.value(), &level)) {
                        return false;
                    }
                }
            }
            filters->append(LogCategoryFilter((String)cat, level));
        }
        return true;
    }

    static bool parseLevel(const JSONValue &value, LogLevel *level) {
        const JSONString name = value.toString();
        static struct {
            const char *name;
            LogLevel level;
        } levels[] = {
                { "none", LOG_LEVEL_NONE },
                { "trace", LOG_LEVEL_TRACE },
                { "info", LOG_LEVEL_INFO },
                { "warn", LOG_LEVEL_WARN },
                { "error", LOG_LEVEL_ERROR },
                { "panic", LOG_LEVEL_PANIC },
                { "all", LOG_LEVEL_ALL }
            };
        static size_t n = sizeof(levels) / sizeof(levels[0]);
        size_t i = 0;
        for (; i < n; ++i) {
            if (name == levels[i].name) {
                break;
            }
        }
        if (i == n) {
            return false; // Unknown level name
        }
        *level = levels[i].level;
        return true;
    }

    static bool processRequest(Request &req, JSONWriter &writer) {
        if (req.cmd == "addHandler") {
            return addHandler(req, writer);
        } else if (req.cmd == "removeHandler") {
            return removeHandler(req, writer);
        } else if (req.cmd == "enumHandlers") {
            return enumHandlers(req, writer);
        } else {
            return false; // Unsupported request
        }
    }

    static bool addHandler(Request &req, JSONWriter&) {
        return logManager()->addFactoryHandler((const char*)req.id, (const char*)req.handler.type, req.level, req.filters,
                req.handler.params, (const char*)req.stream.type, req.stream.params);
    }

    static bool removeHandler(Request &req, JSONWriter&) {
        logManager()->removeFactoryHandler((const char*)req.id);
        return true;
    }

    static bool enumHandlers(Request &req, JSONWriter &writer) {
        writer.beginArray();
        logManager()->enumFactoryHandlers(enumHandlersCallback, &writer);
        writer.endArray();
        return true;
    }

    static void enumHandlersCallback(const char *id, void *data) {
        JSONWriter *writer = static_cast<JSONWriter*>(data);
        writer->value(id);
    }

    static LogManager* logManager() {
        return LogManager::instance();
    }
};

// Custom deleter for std::unique_ptr
template<typename T, typename FactoryT, void(FactoryT::*destroy)(T*)>
struct FactoryDeleter {
    FactoryDeleter() : factory(nullptr) {
    }

    FactoryDeleter(FactoryT *factory) : factory(factory) {
    }

    void operator()(T *ptr) {
        if (ptr) {
            (factory->*destroy)(ptr);
        }
    }

    FactoryT *factory;
};

typedef FactoryDeleter<LogHandler, LogHandlerFactory, &LogHandlerFactory::destroyHandler> LogHandlerFactoryDeleter;
typedef FactoryDeleter<Print, OutputStreamFactory, &OutputStreamFactory::destroyStream> OutputStreamFactoryDeleter;

#if PLATFORM_ID == 3
// GCC on some platforms doesn't provide strchrnul()
inline const char* strchrnul(const char *s, char c) {
    while (*s && *s != c) {
        ++s;
    }
    return s;
}
#endif

// Iterates over subcategory names separated by '.' character
const char* nextSubcategoryName(const char* &category, size_t &size) {
    const char *s = strchrnul(category, '.');
    size = s - category;
    if (size) {
        if (*s) {
            ++s;
        }
        std::swap(s, category);
        return s;
    }
    return nullptr;
}

const char* extractFileName(const char *s) {
    const char *s1 = strrchr(s, '/');
    if (s1) {
        return s1 + 1;
    }
    return s;
}

const char* extractFuncName(const char *s, size_t *size) {
    const char *s1 = s;
    for (; *s; ++s) {
        if (*s == ' ') {
            s1 = s + 1; // Skip return type
        } else if (*s == '(') {
            break; // Skip argument types
        }
    }
    *size = s - s1;
    return s1;
}

} // namespace

// Default logger instance. This code is compiled as part of the wiring library which has its own
// category name specified at module level, so here we use "app" category name explicitly
const spark::Logger spark::Log("app");

/*
    LogFilter instance maintains a prefix tree based on a list of category filter strings. Every
    node of the tree contains a subcategory name and, optionally, a logging level - if node matches
    complete filter string. For example, given the following filters:

    a (error)
    a.b.c (trace)
    a.b.x (trace)
    aa (error)
    aa.b (warn)

    LogFilter builds the following prefix tree:

    |
    |- a (error) -- b - c (trace)
    |               |
    |               `-- x (trace)
    |
    `- aa (error) - b (warn)
*/

// spark::detail::LogFilter
struct spark::detail::LogFilter::Node {
    const char *name; // Subcategory name
    uint16_t size; // Name length
    int16_t level; // Logging level (-1 if not specified for this node)
    Array<Node> nodes; // Children nodes

    Node(const char *name, uint16_t size) :
            name(name),
            size(size),
            level(-1) {
    }
};

spark::detail::LogFilter::LogFilter(LogLevel level) :
        level_(level) {
}

spark::detail::LogFilter::LogFilter(LogLevel level, LogCategoryFilters filters) :
        level_(LOG_LEVEL_NONE) { // Fallback level that will be used in case of construction errors
    // Store category names
    Array<String> cats;
    if (!cats.reserve(filters.size())) {
        return;
    }
    for (LogCategoryFilter &filter: filters) {
        cats.append(std::move(filter.cat_));
    }
    // Process category filters
    Array<Node> nodes;
    for (int i = 0; i < cats.size(); ++i) {
        const char *category = cats.at(i).c_str();
        if (!category) {
            continue; // Invalid usage or string allocation error
        }
        Array<Node> *pNodes = &nodes; // Root nodes
        const char *name = nullptr; // Subcategory name
        size_t size = 0; // Name length
        while ((name = nextSubcategoryName(category, size))) {
            bool found = false;
            const int index = nodeIndex(*pNodes, name, size, found);
            if (!found && !pNodes->insert(index, Node(name, size))) { // Add node
                return;
            }
            Node &node = pNodes->at(index);
            if (!*category) { // Check if it's last subcategory
                node.level = filters.at(i).level_;
            }
            pNodes = &node.nodes;
        }
    }
    using std::swap;
    swap(cats_, cats);
    swap(nodes_, nodes);
    level_ = level;
}

spark::detail::LogFilter::~LogFilter() {
}

LogLevel spark::detail::LogFilter::level(const char *category) const {
    LogLevel level = level_; // Default level
    if (!nodes_.isEmpty() && category) {
        const Array<Node> *pNodes = &nodes_; // Root nodes
        const char *name = nullptr; // Subcategory name
        size_t size = 0; // Name length
        while ((name = nextSubcategoryName(category, size))) {
            bool found = false;
            const int index = nodeIndex(*pNodes, name, size, found);
            if (!found) {
                break;
            }
            const Node &node = pNodes->at(index);
            if (node.level >= 0) {
                level = (LogLevel)node.level;
            }
            pNodes = &node.nodes;
        }
    }
    return level;
}

int spark::detail::LogFilter::nodeIndex(const Array<Node> &nodes, const char *name, size_t size, bool &found) {
    // Using binary search to find existent node or suitable position for new node
    return std::distance(nodes.begin(), std::lower_bound(nodes.begin(), nodes.end(), std::make_pair(name, size),
            [&found](const Node &node, const std::pair<const char*, size_t> &value) {
                const int cmp = strncmp(node.name, value.first, std::min<size_t>(node.size, value.second));
                if (cmp == 0) {
                    if (node.size == value.second) { // Lengths are equal
                        found = true; // Allows caller code to avoid extra call to strncmp()
                        return false;
                    }
                    return node.size < value.second;
                }
                return cmp < 0;
            }));
}

// spark::StreamLogHandler
void spark::StreamLogHandler::logMessage(const char *msg, LogLevel level, const char *category, const LogAttributes &attr) {
    const char *s = nullptr;
    // Timestamp
    if (attr.has_time) {
        printf("%010u ", (unsigned)attr.time);
    }
    // Category
    if (category) {
        write('[');
        write(category);
        write("] ", 2);
    }
    // Source file
    if (attr.has_file) {
        s = extractFileName(attr.file); // Strip directory path
        write(s); // File name
        if (attr.has_line) {
            write(':');
            printf("%d", (int)attr.line); // Line number
        }
        if (attr.has_function) {
            write(", ", 2);
        } else {
            write(": ", 2);
        }
    }
    // Function name
    if (attr.has_function) {
        size_t n = 0;
        s = extractFuncName(attr.function, &n); // Strip argument and return types
        write(s, n);
        write("(): ", 4);
    }
    // Level
    s = levelName(level);
    write(s);
    write(": ", 2);
    // Message
    if (msg) {
        write(msg);
    }
    // Additional attributes
    if (attr.has_code || attr.has_details) {
        write(" [", 2);
        if (attr.has_code) {
            write("code = ", 7);
            printf("%" PRIiPTR, (intptr_t)attr.code);
        }
        if (attr.has_details) {
            if (attr.has_code) {
                write(", ", 2);
            }
            write("details = ", 10);
            write(attr.details);
        }
        write(']');
    }
    write("\r\n", 2);
}

// spark::JSONStreamLogHandler
void spark::JSONStreamLogHandler::logMessage(const char *msg, LogLevel level, const char *category, const LogAttributes &attr) {
    writer_.beginObject();
    // Level
    const char *s = levelName(level);
    writer_.name("level", 5).value(s);
    // Message
    if (msg) {
        writer_.name("message", 7).value(msg);
    }
    // Category
    if (category) {
        writer_.name("category", 8).value(category);
    }
    // File name
    if (attr.has_file) {
        s = extractFileName(attr.file); // Strip directory path
        writer_.name("file", 4).value(s);
    }
    // Line number
    if (attr.has_line) {
        writer_.name("line", 4).value(attr.line);
    }
    // Function name
    if (attr.has_function) {
        size_t n = 0;
        s = extractFuncName(attr.function, &n); // Strip argument and return types
        writer_.name("function", 8).value(s, n);
    }
    // Timestamp
    if (attr.has_time) {
        writer_.name("time", 4).value((unsigned)attr.time);
    }
    // Code
    if (attr.has_code) {
        writer_.name("code", 4).value((int)attr.code);
    }
    // Details
    if (attr.has_details) {
        writer_.name("details", 7).value(attr.details);
    }
    writer_.endObject();
    writer_.stream()->write("\r\n");
}

// spark::DefaultLogHandlerFactory
LogHandler* spark::DefaultLogHandlerFactory::createHandler(const char *type, LogLevel level, LogCategoryFilters filters,
            Print *stream, const JSONValue &params) {
    if (strcmp(type, "JSONStreamLogHandler") == 0) {
        if (!stream) {
            return nullptr; // Output stream is not specified
        }
        return new(std::nothrow) JSONStreamLogHandler(*stream, level, std::move(filters));
    } else if (strcmp(type, "StreamLogHandler") == 0) {
        if (!stream) {
            return nullptr;
        }
        return new(std::nothrow) StreamLogHandler(*stream, level, std::move(filters));
    }
    return nullptr; // Unknown handler type
}

spark::DefaultLogHandlerFactory* spark::DefaultLogHandlerFactory::instance() {
    static DefaultLogHandlerFactory factory;
    return &factory;
}

// spark::DefaultOutputStreamFactory
Print* spark::DefaultOutputStreamFactory::createStream(const char *type, const JSONValue &params) {
#if PLATFORM_ID != 3
    if (strcmp(type, "Serial") == 0) {
        Serial.begin();
        return &Serial;
    }
#if Wiring_USBSerial1
    if (strcmp(type, "USBSerial1") == 0) {
        USBSerial1.begin();
        return &USBSerial1;
    }
#endif
    if (strcmp(type, "Serial1") == 0) {
        int baud = 9600;
        getParams(params, &baud);
        Serial1.begin(baud);
        return &Serial1;
    }
#endif // PLATFORM_ID != 3
    return nullptr;
}

void spark::DefaultOutputStreamFactory::destroyStream(Print *stream) {
#if PLATFORM_ID != 3
    if (stream == &Serial) {
        Serial.end();
        return;
    }
#if Wiring_USBSerial1
    if (stream == &USBSerial1) {
        USBSerial1.end();
        return;
    }
#endif
    if (stream == &Serial1) {
        Serial1.end();
        return;
    }
#endif // PLATFORM_ID != 3
    OutputStreamFactory::destroyStream(stream);
}

spark::DefaultOutputStreamFactory* spark::DefaultOutputStreamFactory::instance() {
    static DefaultOutputStreamFactory factory;
    return &factory;
}

void spark::DefaultOutputStreamFactory::getParams(const JSONValue &params, int *baudRate) {
    JSONObjectIterator it(params);
    while (it.next()) {
        if (it.name() == "baud" && baudRate) {
            *baudRate = it.value().toInt();
        }
    }
}

// spark::LogManager
struct spark::LogManager::FactoryHandler {
    String id;
    LogHandler *handler;
    Print *stream;
};

spark::LogManager::LogManager() :
        handlerFactory_(DefaultLogHandlerFactory::instance()),
        streamFactory_(DefaultOutputStreamFactory::instance()) {
}

spark::LogManager::~LogManager() {
    log_set_callbacks(nullptr, nullptr, nullptr, nullptr); // Reset system callbacks
    WITH_LOCK(mutex_) {
        destroyFactoryHandlers(); // Destroying all dynamically allocated handlers
    }
}

bool spark::LogManager::addHandler(LogHandler *handler) {
    WITH_LOCK(mutex_) {
        if (activeHandlers_.contains(handler) || !activeHandlers_.append(handler)) {
            return false;
        }
        if (activeHandlers_.size() == 1) {
            log_set_callbacks(logMessage, logWrite, logEnabled, nullptr); // Set system callbacks
        }
    }
    return true;
}

void spark::LogManager::removeHandler(LogHandler *handler) {
    WITH_LOCK(mutex_) {
        if (activeHandlers_.removeOne(handler) && activeHandlers_.isEmpty()) {
            log_set_callbacks(nullptr, nullptr, nullptr, nullptr); // Reset system callbacks
        }
    }
}

bool spark::LogManager::addFactoryHandler(const char *id, const char *handlerType, LogLevel level, LogCategoryFilters filters,
        const JSONValue &handlerParams, const char *streamType, const JSONValue &streamParams) {
    WITH_LOCK(mutex_) {
        destroyFactoryHandler(id); // Destroy existent handler with the same ID
        FactoryHandler h;
        h.id = id;
        if (!h.id.length()) {
            return false; // Empty handler ID or memory allocation error
        }
        // Create output stream (optional)
        std::unique_ptr<Print, OutputStreamFactoryDeleter> stream(nullptr, streamFactory_);
        if (streamType) {
            if (streamFactory_) {
                stream.reset(streamFactory_->createStream(streamType, streamParams));
            }
            if (!stream) {
                return false; // Unsupported stream type
            }
        }
        // Create log handler
        std::unique_ptr<LogHandler, LogHandlerFactoryDeleter> handler(nullptr, handlerFactory_);
        if (handlerType && handlerFactory_) {
            handler.reset(handlerFactory_->createHandler(handlerType, level, std::move(filters), stream.get(), handlerParams));
        }
        if (!handler) {
            return false; // Unsupported handler type
        }
        h.handler = handler.get();
        h.stream = stream.get();
        if (!factoryHandlers_.append(std::move(h))) {
            return false;
        }
        if (!activeHandlers_.append(h.handler)) {
            factoryHandlers_.takeLast(); // Revert succeeded factoryHandlers_.append()
            return false;
        }
        handler.release(); // Release scope guard pointers
        stream.release();
    }
    return true;
}

void spark::LogManager::removeFactoryHandler(const char *id) {
    WITH_LOCK(mutex_) {
        destroyFactoryHandler(id);
    }
}

void spark::LogManager::enumFactoryHandlers(void(*callback)(const char *id, void *data), void *data) {
    WITH_LOCK(mutex_) {
        for (const FactoryHandler &h: factoryHandlers_) {
            callback(h.id.c_str(), data);
        }
    }
}

void spark::LogManager::setHandlerFactory(LogHandlerFactory *factory) {
    WITH_LOCK(mutex_) {
        if (handlerFactory_ != factory) {
            destroyFactoryHandlers();
            handlerFactory_ = factory;
        }
    }
}

void spark::LogManager::setStreamFactory(OutputStreamFactory *factory) {
    WITH_LOCK(mutex_) {
        if (streamFactory_ != factory) {
            destroyFactoryHandlers();
            streamFactory_ = factory;
        }
    }
}

void spark::LogManager::destroyFactoryHandler(const char *id) {
    for (int i = 0; i < factoryHandlers_.size(); ++i) {
        const FactoryHandler &h = factoryHandlers_.at(i);
        if (h.id == id) {
            activeHandlers_.removeOne(h.handler);
            handlerFactory_->destroyHandler(h.handler);
            if (h.stream) {
                streamFactory_->destroyStream(h.stream);
            }
            factoryHandlers_.remove(i);
            break;
        }
    }
}

void spark::LogManager::destroyFactoryHandlers() {
    for (const FactoryHandler &h: factoryHandlers_) {
        activeHandlers_.removeOne(h.handler);
        handlerFactory_->destroyHandler(h.handler);
        if (h.stream) {
            streamFactory_->destroyStream(h.stream);
        }
    }
    factoryHandlers_.clear();
}

spark::LogManager* spark::LogManager::instance() {
    static LogManager mgr;
    return &mgr;
}

void spark::LogManager::logMessage(const char *msg, int level, const char *category, const LogAttributes *attr, void *reserved) {
    LogManager *that = instance();
    WITH_LOCK(that->mutex_) {
        for (LogHandler *handler: that->activeHandlers_) {
            handler->message(msg, (LogLevel)level, category, *attr);
        }
    }
}

void spark::LogManager::logWrite(const char *data, size_t size, int level, const char *category, void *reserved) {
    LogManager *that = instance();
    WITH_LOCK(that->mutex_) {
        for (LogHandler *handler: that->activeHandlers_) {
            handler->write(data, size, (LogLevel)level, category);
        }
    }
}

int spark::LogManager::logEnabled(int level, const char *category, void *reserved) {
    LogManager *that = instance();
    int minLevel = LOG_LEVEL_NONE;
    WITH_LOCK(that->mutex_) {
        for (LogHandler *handler: that->activeHandlers_) {
            const int level = handler->level(category);
            if (level < minLevel) {
                minLevel = level;
            }
        }
    }
    return (level >= minLevel);
}

// spark::
bool spark::logProcessRequest(char *buf, size_t bufSize, size_t reqSize, size_t *repSize, DataFormat fmt) {
    if (fmt == DATA_FORMAT_JSON) {
        return JSONRequestHandler::process(buf, bufSize, reqSize, repSize);
    }
    return false; // Unsupported request format
}