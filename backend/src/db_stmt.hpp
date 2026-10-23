#pragma once

// Intern hjälpklass för db.cpp och db_cache.cpp. Ingår inte i Database-klassens gränssnitt.

#include <errmsg.h>
#include <mysql.h>
#include <mysqld_error.h>

#include <stdexcept>
#include <string>
#include <vector>

namespace brevduva {

inline bool connectionLost(unsigned int err) {
    return err == CR_SERVER_GONE_ERROR || err == CR_SERVER_LOST;
}

// Förberedd sats. Parametrar numreras från 1 och kolumner från 0. Alla kolumner hämtas som text.
class Stmt {
public:
    Stmt(MYSQL* db, const std::string& sql) {
        for (int attempt = 0;; ++attempt) {
            stmt_ = mysql_stmt_init(db);
            if (!stmt_) throw std::runtime_error("SQL-fel: " + std::string(mysql_error(db)));
            if (mysql_stmt_prepare(stmt_, sql.data(), sql.size()) == 0) break;
            unsigned int err = mysql_stmt_errno(stmt_);
            std::string msg = mysql_stmt_error(stmt_);
            mysql_stmt_close(stmt_);
            stmt_ = nullptr;
            // Servern stänger vilande anslutningar (wait_timeout). mysql_ping återansluter, sedan ett nytt försök.
            if (attempt == 0 && connectionLost(err) && mysql_ping(db) == 0) continue;
            throw std::runtime_error("SQL-fel: " + msg);
        }
        params_.resize(mysql_stmt_param_count(stmt_));
    }
    ~Stmt() {
        if (stmt_) mysql_stmt_close(stmt_);
    }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

    Stmt& bind(int i, const std::string& v) {
        Param& p = param(i);
        p.type = MYSQL_TYPE_STRING;
        p.text = v;
        return *this;
    }
    Stmt& bind(int i, long long v) {
        Param& p = param(i);
        p.type = MYSQL_TYPE_LONGLONG;
        p.number = v;
        return *this;
    }
    Stmt& bind(int i, int v) { return bind(i, static_cast<long long>(v)); }
    // Binärdata (t.ex. komprimerad text) som inte ska tolkas som text i anslutningens teckenkodning.
    Stmt& bindBlob(int i, const std::string& v) {
        Param& p = param(i);
        p.type = MYSQL_TYPE_BLOB;
        p.text = v;
        return *this;
    }
    Stmt& bind(int i, bool v) { return bind(i, v ? 1LL : 0LL); }

    // Kör en sats som inte returnerar rader. Returnerar false om en unik nyckel redan finns.
    bool run() {
        std::vector<MYSQL_BIND> binds(params_.size());
        for (size_t i = 0; i < params_.size(); ++i) {
            Param& p = params_[i];
            MYSQL_BIND& b = binds[i];
            b.buffer_type = p.type;
            if (p.type == MYSQL_TYPE_STRING || p.type == MYSQL_TYPE_BLOB) {
                p.length = p.text.size();
                b.buffer = p.text.data();
                b.buffer_length = p.length;
                b.length = &p.length;
            } else if (p.type == MYSQL_TYPE_LONGLONG) {
                b.buffer = &p.number;
            }
        }
        if (!binds.empty() && mysql_stmt_bind_param(stmt_, binds.data())) fail();
        if (mysql_stmt_execute(stmt_)) {
            if (mysql_stmt_errno(stmt_) == ER_DUP_ENTRY) return false;
            fail();
        }
        return true;
    }

    // Hämtar nästa rad. Första anropet kör satsen.
    bool row() {
        if (!executed_) {
            executed_ = true;
            if (!run()) fail();
            if (mysql_stmt_store_result(stmt_)) fail();
            columns_.resize(mysql_stmt_field_count(stmt_));
            for (auto& c : columns_) c.buffer.resize(64);
            bindResult();
        }
        int rc = mysql_stmt_fetch(stmt_);
        if (rc == MYSQL_NO_DATA) return false;
        if (rc == 1) fail();
        if (rc == MYSQL_DATA_TRUNCATED) {
            // Längre värden än bufferten hämtas om med en buffert i rätt storlek.
            for (size_t i = 0; i < columns_.size(); ++i) {
                Column& c = columns_[i];
                if (c.isNull || c.length <= c.buffer.size()) continue;
                c.buffer.resize(c.length);
                MYSQL_BIND b{};
                b.buffer_type = MYSQL_TYPE_STRING;
                b.buffer = c.buffer.data();
                b.buffer_length = c.buffer.size();
                b.length = &c.length;
                b.is_null = &c.isNull;
                if (mysql_stmt_fetch_column(stmt_, &b, static_cast<unsigned int>(i), 0)) fail();
            }
            bindResult();
        }
        return true;
    }

    std::string text(int i) const {
        const Column& c = columns_.at(i);
        return c.isNull ? std::string() : std::string(c.buffer.data(), c.length);
    }
    long long int64(int i) const {
        std::string t = text(i);
        return t.empty() ? 0 : std::stoll(t);
    }
    int int32(int i) const { return static_cast<int>(int64(i)); }

    long long insertId() { return static_cast<long long>(mysql_stmt_insert_id(stmt_)); }
    long long affectedRows() { return static_cast<long long>(mysql_stmt_affected_rows(stmt_)); }

private:
    struct Param {
        enum_field_types type = MYSQL_TYPE_NULL;
        std::string text;
        long long number = 0;
        unsigned long length = 0;
    };
    struct Column {
        std::vector<char> buffer;
        unsigned long length = 0;
        my_bool isNull = 0;
    };

    Param& param(int i) {
        if (i < 1 || static_cast<size_t>(i) > params_.size()) throw std::runtime_error("SQL-fel: fel parameterindex");
        return params_[i - 1];
    }

    void bindResult() {
        std::vector<MYSQL_BIND> binds(columns_.size());
        for (size_t i = 0; i < columns_.size(); ++i) {
            Column& c = columns_[i];
            binds[i].buffer_type = MYSQL_TYPE_STRING;
            binds[i].buffer = c.buffer.data();
            binds[i].buffer_length = c.buffer.size();
            binds[i].length = &c.length;
            binds[i].is_null = &c.isNull;
        }
        if (!binds.empty() && mysql_stmt_bind_result(stmt_, binds.data())) fail();
    }

    [[noreturn]] void fail() { throw std::runtime_error("SQL-fel: " + std::string(mysql_stmt_error(stmt_))); }

    MYSQL_STMT* stmt_ = nullptr;
    std::vector<Param> params_;
    std::vector<Column> columns_;
    bool executed_ = false;
};

}  // namespace brevduva
