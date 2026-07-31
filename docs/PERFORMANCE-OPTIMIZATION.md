# Performance Optimization Guide

## 1. Build Performance

### ccache Integration
```cmake
# CMakeLists.txt
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif()
```

**Measurement:**
```bash
time cmake --build build --parallel
ccache -s  # View cache statistics
```

### Ninja Generator
```bash
cmake -G Ninja -B build
ninja -C build -j$(nproc)
```

**Speedup:** 20-40% faster than Make on multi-core systems.

### Parallel Builds
```bash
# CMake
cmake --build build --parallel $(nproc)

# Cargo (already parallel by default)
cargo build --release -j$(nproc)
```

### Precompiled Headers
```cmake
target_precompile_headers(${PROJECT_NAME} PRIVATE
    <QWidget>
    <QString>
    <QNetworkAccessManager>
    <memory>
    <vector>
)
```

### Unity Builds
```cmake
set_target_properties(${PROJECT_NAME} PROPERTIES
    UNITY_BUILD ON
    UNITY_BUILD_BATCH_SIZE 16
)
```

**Trade-off:** Faster builds but harder debugging.

## 2. Runtime Performance

### Lazy Loading

**Qt Resource Loading:**
```cpp
class ResourceCache {
    QPixmap getIcon(const QString& name) {
        if (!cache_.contains(name)) {
            cache_[name] = QPixmap(QString(":/icons/%1.png").arg(name));
        }
        return cache_[name];
    }
private:
    QHash<QString, QPixmap> cache_;
};
```

**Dialog Initialization:**
```cpp
class MainWindow : public QWidget {
    QPointer<SettingsDialog> settingsDialog_;
    
    void showSettings() {
        if (!settingsDialog_) {
            settingsDialog_ = new SettingsDialog(this);
        }
        settingsDialog_->show();
    }
};
```

### Caching Strategies

**Network Response Cache:**
```cpp
class ApiClient {
    struct CacheEntry {
        QByteArray data;
        QDateTime expiry;
    };
    QHash<QString, CacheEntry> cache_;
    
    void cacheResponse(const QString& key, const QByteArray& data, int ttlSecs) {
        cache_[key] = {data, QDateTime::currentDateTime().addSecs(ttlSecs)};
    }
    
    std::optional<QByteArray> getCached(const QString& key) {
        auto it = cache_.find(key);
        if (it != cache_.end() && it->expiry > QDateTime::currentDateTime()) {
            return it->data;
        }
        return std::nullopt;
    }
};
```

**Measurement:**
```cpp
QElapsedTimer timer;
timer.start();
auto result = fetchData();
qDebug() << "Fetch took" << timer.elapsed() << "ms";
```

### Async Operations

**QFuture for Background Work:**
```cpp
#include <QtConcurrent>

QFuture<QByteArray> loadFileAsync(const QString& path) {
    return QtConcurrent::run([path]() {
        QFile file(path);
        file.open(QIODevice::ReadOnly);
        return file.readAll();
    });
}

// Usage
auto future = loadFileAsync("/path/to/file");
auto watcher = new QFutureWatcher<QByteArray>(this);
connect(watcher, &QFutureWatcher<QByteArray>::finished, [watcher]() {
    auto data = watcher->result();
    // Process data on main thread
    watcher->deleteLater();
});
watcher->setFuture(future);
```

**QThreadPool for Reusable Workers:**
```cpp
class ProcessTask : public QRunnable {
    void run() override {
        // Heavy computation
        QMetaObject::invokeMethod(receiver_, "onComplete",
            Qt::QueuedConnection, Q_ARG(Result, result));
    }
};

QThreadPool::globalInstance()->start(new ProcessTask());
```

## 3. Memory Optimization

### Object Pooling
```cpp
template<typename T>
class ObjectPool {
    std::vector<std::unique_ptr<T>> pool_;
    std::vector<T*> available_;
    
public:
    T* acquire() {
        if (available_.empty()) {
            pool_.push_back(std::make_unique<T>());
            return pool_.back().get();
        }
        T* obj = available_.back();
        available_.pop_back();
        return obj;
    }
    
    void release(T* obj) {
        obj->reset();  // Clear state
        available_.push_back(obj);
    }
};
```

### Smart Pointer Usage
```cpp
// Prefer unique_ptr for ownership
std::unique_ptr<Config> config_ = std::make_unique<Config>();

// Use shared_ptr only when needed
std::shared_ptr<Cache> cache_ = std::make_shared<Cache>();

// Qt parent-child for widgets (automatic cleanup)
auto* button = new QPushButton("Click", this);  // 'this' owns button
```

### Leak Detection

**Valgrind (Linux/macOS):**
```bash
valgrind --leak-check=full --show-leak-kinds=all ./AegisyClient
```

**AddressSanitizer:**
```cmake
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(${PROJECT_NAME} PRIVATE
        -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(${PROJECT_NAME} PRIVATE -fsanitize=address)
endif()
```

**Qt Object Tracking:**
```cpp
#ifdef QT_DEBUG
qDebug() << "Live QObjects:" << QObject::findChildren<QObject*>().size();
#endif
```

## 4. UI Responsiveness

### Background Threads
```cpp
class DataLoader : public QThread {
    Q_OBJECT
    void run() override {
        auto data = loadLargeDataset();
        emit dataReady(data);
    }
signals:
    void dataReady(const QVector<Item>& data);
};

// Usage
auto* loader = new DataLoader(this);
connect(loader, &DataLoader::dataReady, this, &Widget::updateUI);
connect(loader, &DataLoader::finished, loader, &QObject::deleteLater);
loader->start();
```

### Progress Indicators
```cpp
class ProgressDialog : public QDialog {
    QProgressBar* progress_;
    
public:
    void setProgress(int current, int total) {
        progress_->setMaximum(total);
        progress_->setValue(current);
        QApplication::processEvents();  // Keep UI responsive
    }
};
```

### Debouncing User Input
```cpp
class SearchBox : public QLineEdit {
    QTimer* debounceTimer_;
    
public:
    SearchBox(QWidget* parent = nullptr) : QLineEdit(parent) {
        debounceTimer_ = new QTimer(this);
        debounceTimer_->setSingleShot(true);
        debounceTimer_->setInterval(300);
        
        connect(this, &QLineEdit::textChanged, [this]() {
            debounceTimer_->start();
        });
        connect(debounceTimer_, &QTimer::timeout, this, &SearchBox::performSearch);
    }
    
signals:
    void performSearch();
};
```

### Virtual Scrolling
```cpp
// For large lists, use QAbstractItemModel with lazy loading
class LazyListModel : public QAbstractListModel {
    int rowCount(const QModelIndex&) const override {
        return totalItems_;  // Can be millions
    }
    
    QVariant data(const QModelIndex& index, int role) const override {
        if (!cache_.contains(index.row())) {
            cache_[index.row()] = loadItem(index.row());  // Load on demand
        }
        return cache_[index.row()];
    }
};
```

## 5. Database Performance

### Indexes
```cpp
QSqlQuery query;
query.exec("CREATE INDEX IF NOT EXISTS idx_timestamp ON events(timestamp)");
query.exec("CREATE INDEX IF NOT EXISTS idx_user_id ON sessions(user_id)");
```

**Measurement:**
```cpp
query.exec("EXPLAIN QUERY PLAN SELECT * FROM events WHERE timestamp > ?");
while (query.next()) {
    qDebug() << query.value(0).toString();  // Check if index is used
}
```

### Query Optimization
```cpp
// Bad: N+1 queries
for (const auto& userId : userIds) {
    query.prepare("SELECT * FROM users WHERE id = ?");
    query.addBindValue(userId);
    query.exec();
}

// Good: Single query with IN clause
QString placeholders = QString("?,").repeated(userIds.size());
placeholders.chop(1);
query.prepare(QString("SELECT * FROM users WHERE id IN (%1)").arg(placeholders));
for (const auto& id : userIds) {
    query.addBindValue(id);
}
query.exec();
```

### WAL Mode
```cpp
QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE");
db.setDatabaseName("aegisy.db");
db.open();

QSqlQuery query(db);
query.exec("PRAGMA journal_mode=WAL");  // Write-Ahead Logging
query.exec("PRAGMA synchronous=NORMAL");
query.exec("PRAGMA cache_size=-64000");  // 64MB cache
```

**Benefit:** 10-100x faster writes, concurrent reads.

### Prepared Statements
```cpp
QSqlQuery query;
query.prepare("INSERT INTO logs (level, message, timestamp) VALUES (?, ?, ?)");

for (const auto& log : logs) {
    query.addBindValue(log.level);
    query.addBindValue(log.message);
    query.addBindValue(log.timestamp);
    query.exec();
}
```

### Batch Inserts
```cpp
db.transaction();
for (const auto& item : items) {
    query.exec();  // Execute prepared statement
}
db.commit();  // Single commit for all inserts
```

## 6. Network Performance

### Connection Pooling
```cpp
class NetworkManager {
    QNetworkAccessManager* nam_;
    
public:
    NetworkManager() {
        nam_ = new QNetworkAccessManager(this);
        nam_->setTransferTimeout(30000);
        
        // Connection reuse is automatic in Qt
        // Configure connection limits
        QNetworkConfiguration config;
        config.setConnectTimeout(5000);
    }
};
```

### Compression
```cpp
QNetworkRequest request(url);
request.setRawHeader("Accept-Encoding", "gzip, deflate");

auto* reply = nam_->get(request);
connect(reply, &QNetworkReply::finished, [reply]() {
    QByteArray data = reply->readAll();
    if (reply->hasRawHeader("Content-Encoding")) {
        data = qUncompress(data);  // Qt handles gzip automatically
    }
});
```

### Request Batching
```cpp
class BatchedApiClient {
    QVector<Request> pendingRequests_;
    QTimer* batchTimer_;
    
public:
    void queueRequest(const Request& req) {
        pendingRequests_.append(req);
        if (!batchTimer_->isActive()) {
            batchTimer_->start(100);  // Batch window
        }
    }
    
private slots:
    void sendBatch() {
        if (pendingRequests_.isEmpty()) return;
        
        QJsonArray batch;
        for (const auto& req : pendingRequests_) {
            batch.append(req.toJson());
        }
        
        sendBatchRequest(batch);
        pendingRequests_.clear();
    }
};
```

### HTTP/2 Multiplexing
```cpp
// Qt 5.14+ supports HTTP/2 automatically
request.setAttribute(QNetworkRequest::Http2AllowedAttribute, true);
```

## 7. Profiling Tools

### Instruments (macOS)
```bash
# Time Profiler
instruments -t "Time Profiler" -D trace.trace ./AegisyClient.app

# Allocations
instruments -t "Allocations" -D alloc.trace ./AegisyClient.app

# System Trace
instruments -t "System Trace" -D sys.trace ./AegisyClient.app
```

**Analysis:** Open `.trace` files in Instruments.app to identify hotspots.

### perf (Linux)
```bash
# Record
perf record -g ./AegisyClient

# Report
perf report

# Flamegraph
perf script | stackcollapse-perf.pl | flamegraph.pl > flame.svg
```

### Valgrind Callgrind
```bash
valgrind --tool=callgrind ./AegisyClient
kcachegrind callgrind.out.*
```

### Tracy Profiler
```cpp
// Add Tracy to CMakeLists.txt
find_package(Tracy CONFIG)
target_link_libraries(${PROJECT_NAME} PRIVATE Tracy::TracyClient)

// Instrument code
#include <tracy/Tracy.hpp>

void expensiveFunction() {
    ZoneScoped;  // Automatic scope profiling
    // ... work ...
}
```

**Run:**
```bash
./Tracy  # Start Tracy server
./AegisyClient  # Run instrumented app
```

### Qt Creator Profiler
```bash
# Build with debug symbols
cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo -B build
cmake --build build

# Profile in Qt Creator: Analyze > QML Profiler / Performance Analyzer
```

### Custom Timing
```cpp
class ScopedTimer {
    QElapsedTimer timer_;
    QString name_;
public:
    ScopedTimer(const QString& name) : name_(name) {
        timer_.start();
    }
    ~ScopedTimer() {
        qDebug() << name_ << "took" << timer_.elapsed() << "ms";
    }
};

// Usage
void processData() {
    ScopedTimer timer("processData");
    // ... work ...
}
```

## 8. Benchmarking Strategies

### Qt Benchmark Framework
```cpp
#include <QtTest/QTest>

class BenchmarkTest : public QObject {
    Q_OBJECT
private slots:
    void benchmarkStringConcat() {
        QBENCHMARK {
            QString result;
            for (int i = 0; i < 1000; ++i) {
                result += QString::number(i);
            }
        }
    }
    
    void benchmarkStringBuilder() {
        QBENCHMARK {
            QStringBuilder result;
            for (int i = 0; i < 1000; ++i) {
                result % QString::number(i);
            }
        }
    }
};

QTEST_MAIN(BenchmarkTest)
```

**Run:**
```bash
./BenchmarkTest -iterations 100
```

### Cargo Bench (Rust Components)
```rust
// agent-runtime/benches/protocol_bench.rs
use criterion::{black_box, criterion_group, criterion_main, Criterion};

fn parse_message_benchmark(c: &mut Criterion) {
    c.bench_function("parse_aap_message", |b| {
        let data = generate_test_message();
        b.iter(|| parse_message(black_box(&data)));
    });
}

criterion_group!(benches, parse_message_benchmark);
criterion_main!(benches);
```

**Run:**
```bash
cd agent-runtime
cargo bench
```

### Load Testing
```cpp
class LoadTest {
    void simulateConcurrentUsers(int count) {
        QVector<QThread*> threads;
        for (int i = 0; i < count; ++i) {
            auto* thread = QThread::create([this]() {
                for (int j = 0; j < 100; ++j) {
                    performApiCall();
                    QThread::msleep(100);
                }
            });
            threads.append(thread);
            thread->start();
        }
        
        for (auto* thread : threads) {
            thread->wait();
            delete thread;
        }
    }
};
```

### Regression Testing
```bash
#!/bin/bash
# benchmark.sh

echo "Running benchmarks..."
./BenchmarkTest -o baseline.txt

# After changes
./BenchmarkTest -o current.txt

# Compare
python3 compare_benchmarks.py baseline.txt current.txt
```

### Memory Benchmarking
```cpp
void measureMemoryUsage() {
    QProcess proc;
    proc.start("ps", QStringList() << "-o" << "rss=" << "-p" 
               << QString::number(QCoreApplication::applicationPid()));
    proc.waitForFinished();
    qint64 rssKb = proc.readAllStandardOutput().trimmed().toLongLong();
    qDebug() << "Memory usage:" << rssKb / 1024 << "MB";
}
```

### Startup Time Measurement
```cpp
// main.cpp
int main(int argc, char *argv[]) {
    QElapsedTimer startupTimer;
    startupTimer.start();
    
    QApplication app(argc, argv);
    
    qDebug() << "App init:" << startupTimer.elapsed() << "ms";
    
    MainWindow window;
    window.show();
    
    qDebug() << "Window shown:" << startupTimer.elapsed() << "ms";
    
    return app.exec();
}
```

## Performance Checklist

- [ ] Enable ccache for C++ builds
- [ ] Use Ninja generator for faster builds
- [ ] Add precompiled headers for common Qt includes
- [ ] Implement lazy loading for dialogs and resources
- [ ] Cache expensive computations and network responses
- [ ] Move I/O operations to background threads
- [ ] Use object pooling for frequently allocated objects
- [ ] Enable WAL mode for SQLite databases
- [ ] Add indexes to frequently queried columns
- [ ] Batch database inserts in transactions
- [ ] Implement connection pooling for network requests
- [ ] Enable HTTP/2 for API calls
- [ ] Profile with platform-specific tools (Instruments/perf)
- [ ] Add benchmarks for critical paths
- [ ] Monitor memory usage in production
- [ ] Set performance budgets and regression tests

## Platform-Specific Optimizations

### macOS
```cmake
if(APPLE)
    target_compile_options(${PROJECT_NAME} PRIVATE
        -O3 -march=native -flto)
    target_link_options(${PROJECT_NAME} PRIVATE -flto)
endif()
```

### Windows
```cmake
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        /O2 /GL /arch:AVX2)
    target_link_options(${PROJECT_NAME} PRIVATE /LTCG)
endif()
```

### Rust (agent-runtime)
```toml
[profile.release]
opt-level = 3
lto = "fat"
codegen-units = 1
strip = true
```

## References

- [Qt Performance Tips](https://doc.qt.io/qt-6/performance.html)
- [CMake Build Performance](https://cmake.org/cmake/help/latest/manual/cmake-buildsystem.7.html)
- [Cargo Performance](https://doc.rust-lang.org/cargo/reference/profiles.html)
- [SQLite Performance Tuning](https://www.sqlite.org/pragma.html)
