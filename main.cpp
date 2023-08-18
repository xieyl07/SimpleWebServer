#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cassert>
#include <cstdarg>
#include <csignal>
#include <cerrno>

#include <vector>
#include <string>
#include <forward_list>
#include <list>
#include <deque>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <thread>
#include <iostream>
#include <functional>
#include <tuple>

#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
#include <fcntl.h>
#include <mysql/mysql.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <netinet/in.h>
#include <arpa/inet.h>

using std::cin;
using std::cout;
using std::cerr;
using std::endl;
using std::vector;
using std::string;
using std::list;
using std::deque;
using std::queue;
using std::unordered_map;
using std::thread;
using std::function;
using std::tuple;

enum ActorModel {REACTOR, PROACTOR};
enum LogLevel {DEBUG, INFO, WARNING, ERROR};
enum TrigMode {EDGE, LEVEL};
enum METHOD {GET, POST};
enum LINE_STATE {OPEN, FIND_R, FIND};
enum PARSE_STATE {BAD, FINE, FINISH};

static const char * response_200 = "HTTP/1.1 200 OK\r\n";
static const char * response_301 = "HTTP/1.1 301 Moved Permanently\r\n";
static const char * response_302 = "HTTP/1.1 302 Found\r\n";
static const char * response_400 = "HTTP/1.1 400 Bad Request\r\n";
static const char * response_403 = "HTTP/1.1 403 Forbidden\r\n";
static const char * response_404 = "HTTP/1.1 404 Not Found\r\n";
static const char * response_500 = "HTTP/1.1 500 Internal Server Error\r\n";

/* 成员函数之间不空行, private前面空一行, 适当使用空行分组
 * 可以调整的值都放到tuning里面
 */

// TODO
// 来个LRU 或者时钟算法缓存页面吧, 多好
// log还要改, 输出到屏幕和不能丢数据, 两个改成队列

void err_print(int line_num, const char* func_name, const char *format, ...) {
    fprintf(stderr, "%s(%d)=> ", func_name, line_num);
    va_list va;
    va_start(va, format);
    vfprintf(stderr, format, va);
    va_end(va);
    fprintf(stderr, "\n");
}

void response_print(int line_num, const char* func_name, const char *response_statement, const char* format, ...) {
    fprintf(stderr, "%s(%d)=> %s", func_name, line_num, response_statement);
    va_list va;
    va_start(va, format);
    vfprintf(stderr, format, va);
    va_end(va);
    fprintf(stderr, " <=%s(%d)\n", func_name, line_num);
}

struct Tuning {
    // 可以通过命令参数修改的
    // 检查: 名字, 类型, 默认值
    int port = 8011;
    bool so_linger = false; // socket opt, linger after close(3)
    int thread_num = 16; // threads number of thread pool
    int sql_num = thread_num; // max connection number of database
    TrigMode connfd_trig = EDGE;
    ActorModel actor_model = REACTOR;
    bool enable_log = true;
    bool async_log = true;
    LogLevel log_lv = INFO;

    // 不能通过命令参数修改(没调整成可以通过命令行参数修改), 要重新编译的
#   define REC_BUF_LEN 4096 /* http读缓存 */
#   define SEND_BUF_LEN 4096 /* http写缓存 */
    static constexpr int slot_num = 5; // 计时器
    static constexpr struct timeval tick_time = {1, 0}; // 计时器
    static constexpr int MAX_EVENTS_NUM = 100000; // Server
# define ROOT_PATH "/home/c/root" /* HttpConn */
    const struct rlimit nofile_limit = {150000, 200000}; // upscale_limit
    const struct rlimit nice_limit {25, 25}; // upscale_limit
#   define NEWSOMAXCONN 40960 /* upscale_limit */
    static constexpr int block_size = 40960; // log. 因为log的设计只有两块, 内存读写肯定比硬盘快, 所以小了会丢日志
    const string log_path = "/home/c/log/server.log"; // log
#   define user "root" /* mysql */
#   define passwd "" /* mysql */
#   define db_name "serverdb" /* mysql */
#   define mysql_ip "localhost" /* mysql */
#   define MAX_EVENTS 18 /* epoll_event */
};

Tuning tuning;
int epfd;

class Runable {
 public:
    virtual void run(void*) = 0;
};

class NoCopy {
 public:
    NoCopy() = default;
    NoCopy(const NoCopy&) = delete;
    NoCopy& operator=(const NoCopy&) = delete;
};

struct HttpConn {
    int connfd;

    // buffer
    char rec_buf[REC_BUF_LEN];
    char *idx_to_check = rec_buf; // 每次在行首, 读取一点加一点, 加到下一个行首
    char *rec_idx = rec_buf;
    char send_buf[SEND_BUF_LEN];
    char *idx_to_write = send_buf;
    char *send_idx = send_buf;

    // parse result
    int line_num_parsed = 0;
    char *line_end;
    bool empty_line = false;
    LINE_STATE line_status = OPEN;
    METHOD method;
    const char *response_code;
    string root_path = ROOT_PATH;
    string file_path;
    string full_path; // root_path + file_path
    string connection = ""; // keep-alive
    string cookie = "";
    long post_content_length = 0;

    // response words
    const char *content_type = "text/html; charset=utf-8";
    string set_cookie = "";
    string location = "";
    int file_fd; // 要记得关闭
    char *file_map; // 这个也要发送
    int map_len;
    int sent_len = 0;

    int mag;
};

class Util {
 public:
    static void arg_parse(int argc, char *argv[]);
    static int set_noblock(int fd);
    static int add_sig(int sig, void (*handler)(int), bool restart = false);
    static int add_fd(int epfd, int fd,
                      int epoll_in_out, TrigMode trig_mode, bool oneshot);
    static int mod_fd(int epfd, int fd,
                      int epoll_in_out, TrigMode trig_mode, bool oneshot);
    static void upscale_limit();
    static void finish(int);
    static MYSQL* get_sql_conn();
    static int return_sql_conn(MYSQL *conn);
    static void init(HttpConn *http_conn);
    static bool recv(HttpConn *conn);
    static void deal_out(HttpConn *conn, MYSQL*);
    static void clean(HttpConn *conn);

 private:
    static bool send(HttpConn *conn);
};

bool met_pipe = false;

/***************** lock *****************/

class Sem {
 public:
    Sem() {
        if (sem_init(&sem_, 0, 0) != 0) {
            //error handle
        }
    }
    ~Sem() {
        sem_destroy(&sem_);
    }
    bool wait() {
        return sem_wait(&sem_) == 0;
    }
    bool post() {
        return sem_post(&sem_) == 0;
    }

 private:
    sem_t sem_;
};

class Mutex {
 public:
    Mutex() = default;
    ~Mutex() {
        pthread_mutex_destroy(&mutex_);
    }
    bool lock() {
        return pthread_mutex_lock(&mutex_) == 0;
    }
    bool unlock() {
        return pthread_mutex_unlock(&mutex_) == 0;
    }

 private:
    // 这种用macro初始化的写法, 区别就是没有错误检测
    pthread_mutex_t mutex_ = PTHREAD_MUTEX_INITIALIZER;
};

class RwLock {
 public:
    RwLock() = default;
    ~RwLock() {
        pthread_rwlock_destroy(&rwlock_);
    }
    bool rdlock() {
        return pthread_rwlock_rdlock(&rwlock_) == 0;
    }
    bool wrlock() {
        return pthread_rwlock_wrlock(&rwlock_) == 0;
    }
    bool unlock() {
        return pthread_rwlock_unlock(&rwlock_) == 0;
    }

 private:
    pthread_rwlock_t rwlock_ = PTHREAD_RWLOCK_INITIALIZER;
};

class Cond {
 public:
    Cond() {
        if (pthread_mutex_init(&mutex_, NULL) != 0) {
            //error handle
        }
        if (pthread_cond_init(&cond_, NULL)) {
            pthread_mutex_destroy(&mutex_);
            //error handle
        }
    }
    ~Cond() {
        pthread_mutex_destroy(&mutex_);
        pthread_cond_destroy(&cond_);
    }
    bool signal() {
        return pthread_cond_signal(&cond_) == 0;
    }
    bool broadcast() {
        return pthread_cond_broadcast(&cond_) == 0;
    }
    bool wait() {
        pthread_mutex_lock(&mutex_);
        int result = pthread_cond_wait(&cond_, &mutex_);
        pthread_mutex_unlock(&mutex_);
        return result == 0;
    }

 private:
    pthread_mutex_t mutex_;
    pthread_cond_t cond_;
};


/***************** timer wheel *****************/

//struct TimerNode {
//    using callback_t = void(*)();
//    int remaining_round = -1;
//    callback_t cb = nullptr;
//};
//
//class TimerWheel {
//    using callback_t = void(*)();
// public:
//    TimerWheel()
//      : slot_num(tuning.slot_num), tick_time(tuning.tick_time),
//        us_tick(tick_time.tv_sec * 1000000 + tick_time.tv_usec),
//        us_per_round(us_tick * slot_num), slots_(slot_num)
//    {
//        ;
//    }
//    void add_timer(struct timeval remaining_tv, callback_t cb) {
//        long long remaining_us = remaining_tv.tv_sec * 1000000 + remaining_tv.tv_usec;
//        int remaining_round = remaining_us / us_per_round;
//        int slot_idx = (idx_ + remaining_us % us_per_round / us_tick) % slot_num;
//        auto &timer_list = slots_[slot_idx];
//        auto iter = timer_list.begin();
//        while (iter != timer_list.end() && iter->remaining_round < remaining_round) ++iter;
//        timer_list.insert(iter, {remaining_round, cb});
//    }
//    void tick() {
//        ++idx_ %= slot_num;
//        auto &timer_list = slots_[idx_];
//        for (auto iter = timer_list.begin(); iter != timer_list.end(); ++iter) {
//            if (iter->remaining_round-- == 0) {
//                iter->cb;
//                iter = timer_list.erase(iter);
//            }
//        }
//    }
//
// private:
//    // 由 tuning 控制的常量
//    const int slot_num; // 为了初始化列表, slot_num放vector前面!
//    const struct timeval tick_time;
//    const long long us_tick;
//    const long long us_per_round;
//
//    int idx_ = 0;
//    vector<list<TimerNode>> slots_;
//};


/***************** log *****************/

class Log {
 public:
    static Log& get_instance(){
        static Log log;
        return log;
    }
    void do_log(LogLevel lv, const char *format, ...) {
        if (!enable_log || lv < min_log_lv) return;

        const char *lv_str;
        switch (lv) {
        case DEBUG:
            lv_str = "DEBUG";
            break;
        case INFO:
            lv_str = "INFO";
            break;
        case WARNING:
            lv_str = "WARNING";
            break;
        case ERROR:
            lv_str = "ERROR";
            break;
        }

        struct timeval tv = {0};
        gettimeofday(&tv, nullptr);
        struct tm *sys_tm = localtime(&tv.tv_sec);

        va_list va;
        va_start(va, format);

        if (async_log) {
            // 别的不说, 写入blocks当然要上锁
            mtx.lock();
            // 检查是不是需要n printf
            // printf 返回的长度不包含'\0'
            int len0 = sprintf(str0,"%d-%02d-%02d %02d:%02d:%02d.%06ld %s ",
                               sys_tm->tm_year + 1900, sys_tm->tm_mon + 1,
                               sys_tm->tm_mday, sys_tm->tm_hour, sys_tm->tm_min,
                               sys_tm->tm_sec, tv.tv_usec, lv_str);
            // 拒绝麻烦的字符串分割写入, 剩下部分写不下了就换一块
            if (block_size - write_idx - 1 < len0) {
                ++block_write_idx %= 2;
                notify.signal();
                write_idx = 0;
            }
            write_idx += sprintf(blocks[block_write_idx] + write_idx, str0);

            // 就算是截断的写法, 也要先写到一个小字符串里.
            // 不然断了从哪里开始, va_list又不能从中间继续
            int len_format = vsnprintf(str_format, 255, format, va);
            if (len_format == 255) {
                // error handle
            }
            len_format += sprintf(str_format + len_format, "\n");
            if (block_size - write_idx -1 < len_format) {
                ++block_write_idx %= 2;
                notify.signal();
                write_idx = 0;
            }
            write_idx += sprintf(blocks[block_write_idx] + write_idx, str_format);
            mtx.unlock();
        } else { // sync write
            // 因为c标准库的io有缓冲区, 每次fprintf/fwrite写入不涉及系统调用,
            // 所以多次fprintf而不是都写入到一个字符串里再fprintf.
            // 缓冲区彼此独立所以不用上锁, 很安全
            FILE *fp = fopen(log_path.c_str(), "a");
            fprintf(fp, "%d-%02d-%02d %02d:%02d:%02d.%06ld %s",
                    sys_tm->tm_year + 1900, sys_tm->tm_mon + 1,
                    sys_tm->tm_mday, sys_tm->tm_hour, sys_tm->tm_min,
                    sys_tm->tm_sec, tv.tv_usec, lv_str);

            vfprintf(fp, format, va);
            fprintf(fp, "\n");
            fclose(fp);
        }
        va_end(va);
    }

    bool stop = false;
    Cond notify;

 private:
    Log() : enable_log(tuning.enable_log) {
        if (!enable_log) return;
        async_log = tuning.async_log;
        min_log_lv = tuning.log_lv;
        block_size = tuning.block_size;
        log_path = tuning.log_path;
        if (async_log) {
            blocks[0] = new char[block_size];
            blocks[1] = new char[block_size];
            str0 = new char[36];
            str_format = new char[256];
            thread t(thread_write);
            t.detach();
        }
    }
    static void thread_write() {
        Log &log = Log::get_instance();
        FILE *fp = fopen(log.log_path.c_str(), "a");
        while (!log.stop) {
            log.notify.wait();
            if (log.stop) break;
            // 考虑到写入速度, 不上锁了也没事吧
            int block_read_idx = log.block_write_idx ^ 1;
            fprintf(fp, log.blocks[block_read_idx]);
        }

        // 扫尾工作
        fprintf(fp, log.blocks[log.block_write_idx]);
        fclose(fp);
        _exit(0);
    }

    // 由 tuning 控制的常量
    bool enable_log;
    bool async_log;
    LogLevel min_log_lv;
    int block_size;
    string log_path; // 包含文件名, 全部写到一个文件里

    char *blocks[2];
    int block_write_idx = 0; // 选择哪个block写入
    int write_idx = 0; // 当前block写入的下标
    char *str0; // 每一行的日期和等级部分
    char *str_format;
    bool async_write_finish = true;
    Mutex mtx;
    RwLock rwlock;
};

#define LOG_DEBUG(format, ...) \
    Log::get_instance().do_log(DEBUG, format, ##__VA_ARGS__);
#define LOG_INFO(format, ...) \
    Log::get_instance().do_log(INFO, format, ##__VA_ARGS__);
#define LOG_WARNING(format, ...) \
    Log::get_instance().do_log(WARNING, format, ##__VA_ARGS__);
#define LOG_ERROR(format, ...) \
    Log::get_instance().do_log(ERROR, format, ##__VA_ARGS__);


/***************** sql connection pool *****************/


//class SqlConnPool : public NoCopy {
// public:
//    void destory() { // singleton存活至程序结束, 析构函数无所谓的
//        // close connection, Clean up resources
//        mtx.lock();
//        for (MYSQL *conn : conn_pool_) {
//            mysql_close(conn);
//        }
//        conn_pool_.clean();
//        mtx.unlock();
//    }
//    static SqlConnPool& get_instance() {
//        static SqlConnPool conn_pool;
//        return conn_pool;
//    }
//    void init() {
//        sql_num = tuning.sql_num;
//
//        for (int i = 0; i < sql_num; ++i) {
//            MYSQL *conn = mysql_init(NULL);
//            if (!conn) {
//                LOG_WARNING("mysql init error: %s", mysql_errno(conn));
//                continue;
//            }
//            auto ret = mysql_real_connect(conn, tuning.mysql_ip,
//                                          tuning.user, tuning.passwd,
//                                          tuning.db_name,
//                                          MYSQL_PORT,
//                                          NULL, 0);
//            if (ret == NULL) {
//                LOG_WARNING("mysql connect error: %s", mysql_error(conn));
//                mysql_close(conn);
//                continue;
//            }
//            conn_pool_.push_front(conn);
//        }
//        if (conn_pool_.empty()) {
//            LOG_ERROR("sql init error, exit. do you have root privilege?");
//            exit(1);
//        }
//    }
//    // get a sql connection
//    MYSQL* give_conn() {
//        mtx.lock();
//        while (conn_pool_.empty()) {
//            mtx.unlock();
//            notify_.wait(); // 当队列为空时, 阻塞等通知, 不是return NULL就完事
//            mtx.lock();
//        }
//        MYSQL *conn = conn_pool_.front();
//        conn_pool_.pop_front();
//        mtx.unlock();
//        return conn;
//    }
//    // fetch a connection. MUST CALL AFTER USE!
//    void fetch_conn(MYSQL *conn) {
//        if (!conn) return;
//        mtx.lock();
//        conn_pool_.push_front(conn);
//        mtx.unlock();
//        notify_.signal();
//    }
//
// private:
//    SqlConnPool() = default;
//    int sql_num;
//    std::forward_list<MYSQL*> conn_pool_;
//    Mutex mtx;
//    Cond notify_;
//};
//
//#define SQL_CONN_GET() SqlConnPool::get_instance().give_conn();
//#define SQL_CONN_FETCH(conn) SqlConnPool::get_instance().fetch_conn(conn);


/***************** thread pool *****************/

class ThreadPool {
    using func_t = std::pair<void(*)(HttpConn*, MYSQL*), HttpConn*>;
 public:
    static ThreadPool& get_instance() {
        static ThreadPool pool;
        if (pool.stop) {
            pool.stop = false;
            pool.start_pool();
        }
        cerr << __LINE__ << endl;
        return pool;
    }
    void start_pool() {
        auto thread_func = [this]() {
            cerr << __LINE__ << endl;
            MYSQL *conn = Util::get_sql_conn();
//            MYSQL *conn = nullptr; // ###for test
            while (!stop) {
                // 上来就检查tasks, 非空就执行
                // 不要有了通知才运行.
                // 因为通知的时候可能没人有空, 所以没人收到
                cerr << __LINE__ << endl;
                mtx.lock();
                while (tasks.empty()) {
                    mtx.unlock();
                    notify.wait();
                    mtx.lock();
                }
                cerr << __LINE__ << endl;
                if (stop) {
                    mtx.unlock();
                    break;
                }
// 上来先等通知, 通知到了再执行. 这样不好
//                notify.wait();
//                if (stop) break;
//                mtx.lock();
//                if (tasks.empty()) {
//                    mtx.unlock();
//                    continue;
//                }
                func_t obj = tasks.front();
                tasks.pop();
                mtx.unlock();
                cerr << __LINE__ << endl;
                obj.first(obj.second, conn);
            }
            Util::return_sql_conn(conn);
        };

        // start_pool 就这么一个for
        for (int i = 0; i < threads_num; ++i) {
            cerr << __LINE__ << endl;
            threads[i] = thread(thread_func);
        }
    }
    void stop_pool() {
        stop = true;
        notify.broadcast();
        for (int i = 0; i <threads_num; ++i) {
            threads[i].join();
        }
    }
    int add_task(const func_t &obj) {
        mtx.lock();
        tasks.push(obj);
        mtx.unlock();
        notify.signal();
        cerr << __LINE__ << endl;
        return 0;
    }

    bool stop = true;
    Cond notify;

 private:
    ThreadPool() = default;

    int threads_num = tuning.thread_num;
    queue<func_t> tasks;
    thread* threads  = new thread[threads_num];
    Mutex mtx;
};


/***************** http *****************/

// HttpProcessor当成一个传入HttpConn的处理的函数, 结果写在HttpConn的某一格里
// processor 是无状态的
class HttpProcessor {
 public:
    static void run(HttpConn* conn, MYSQL *mysql_conn) {
        if (tuning.actor_model == REACTOR) {
            bool result = Util::recv(conn);
            if (!result) {
                Util::clean(conn);
                close(conn->connfd);
                return;
            }
        }

        // root_path 不能在解析process_req_line做, 不然解析之前就炸了怎么办
        if (!conn->root_path.empty() && conn->root_path.back() != '/') {
            conn->root_path.push_back('/');
        }

        bool is_end = false;
        LINE_STATE &line_state = conn->line_status;

        // 只有 FIND 才可能没走到底
        do {
            line_state = parse_line(conn, line_state, is_end);
            if (line_state != FIND) break;
            if (conn->empty_line && conn->method == POST) {
                char str[100];
                int len = conn->post_content_length > 100 - 1 ? fprintf(stderr, "to long\n"), 99 : conn->post_content_length;
                snprintf(str, len + 1, conn->idx_to_check);
                err_print(__LINE__, __FUNCTION__, "%s", str);
            } else {
                err_print(__LINE__, __FUNCTION__, "%s", conn->idx_to_check);
            }

            if (!conn->empty_line) {
                bool result;
                if (conn->line_num_parsed == 1) { // request line
                    result = process_req_line(conn);
                    if (!result) { // process_req_line 中已经设定了 response
                        response(conn);
                        return;
                    }
                } else { // header
                    result = process_header(conn);
                    if (!result) {
                        response(conn);
                        return;
                    }
                }
            } else { // \r\n\r\n
                fprintf(stderr, "in empty line\n");
                if (conn->method == GET) {
                    conn->response_code = response_200;
                    response(conn);
                    return;
                } else if (conn->rec_idx - conn->idx_to_check < conn->post_content_length) {
                    break;
                } else { // post body
                    // 第二次进, parse_line 开绿灯, 够走这里, 不够 break
                    err_print(__LINE__, __FUNCTION__ , "get and process hole post body");
                    process_body(conn, is_end, mysql_conn); // 解析完整的一行, 不用费心了
                    response(conn);
                    return;
                }
            }
            conn->line_status = OPEN;
        } while (!is_end);
    }

 private:
    static LINE_STATE parse_line(HttpConn *conn, LINE_STATE cur_state, bool &is_end) {
        if (conn->empty_line) return FIND; // 为了第二次能进 do

        char *idx = conn->idx_to_check;
        char *end = conn->rec_idx;

        while (idx < end) {
            switch (cur_state) {
            case OPEN: {
                // 读到底结束. 看不到尽头的parse, 结束是种解脱
                idx = std::find(idx, end, '\r');
                if (idx == end) {
                    break;
                }
                ++idx;
                cur_state = FIND_R;
                break;
            }
            case FIND_R: {
                char next_c = *idx++;
                if (next_c == '\n') {
                    if (idx - 4 == conn->line_end) { // 读完最后一行(\r\n\r\n)了, 不需要is_end, 马上就出循环
                        conn->empty_line = true;
                        err_print(__LINE__, __FUNCTION__ , "set empty line");
                        if (conn->method == POST) {
                            err_print(__LINE__, __FUNCTION__ , "%d", conn->rec_idx - idx);
                            conn->idx_to_check += 2; // 第一次就数据够的话, FIND 出去, 两个 else 到 process_body 出去
                            if (conn->rec_idx < idx + conn->post_content_length) { // 第一次不够的话, OPEN 出去直接 break 等下次
                                err_print(__LINE__, __FUNCTION__ , "");
                                // 要 return 了, 把落下的补上
                                *(conn->line_end = idx - 2) = '\0';
                                ++conn->line_num_parsed;
                                return OPEN; // 长度不够直接退出循环
                            }
                        }
                    } else if (idx == end) {
                        is_end = true;
                    }
                    *(conn->line_end = idx - 2) = '\0';
                    ++conn->line_num_parsed;
                    return FIND;
                } else if (next_c == '\r') {
                    cur_state = FIND_R;
                } else {
                    cur_state = OPEN;
                }
                break;
            }
            case FIND:
                return FIND;
            }
        }
        return cur_state;
    }
    static bool process_req_line(HttpConn *conn) {
        fprintf(stderr, "%s(%d): attention!\n", __FUNCTION__, __LINE__);
        // GET, POST等method以外都不区分大小写
        if (strncmp(conn->idx_to_check, "GET /", 5) == 0) {
            conn->method = GET;
            conn->idx_to_check += 5;
        } else if (strncmp(conn->idx_to_check, "POST /", 6) == 0) {
            conn->method = POST;
            conn->idx_to_check += 6;
        } else {
            LOG_WARNING(conn->rec_buf);
            response_code_set(conn, 400);
            fprintf(stderr, "%s(%d): 400 bad set, idx_to_check: %s\n", __FUNCTION__, __LINE__, conn->idx_to_check);
            return false;
        }

        if (strncasecmp(conn->line_end - 9, " HTTP/1.1", 9) != 0) {
            LOG_WARNING(conn->rec_buf);
            response_code_set(conn, 400);
            fprintf(stderr, "400 Bad: %d\n", __LINE__);
            return false;
        }
        *(conn->line_end - 9) = '\0'; // " HT..." -> "'\0'HT..."

        conn->file_path = conn->idx_to_check;
        if (conn->method == GET) {
            if (conn->file_path.empty()) {
                conn->file_path = "index.html";
            }
            conn->full_path = conn->root_path + conn->file_path;
            struct stat st = {0};
            if (stat(conn->full_path.c_str(), &st) != 0 ||
                !S_ISREG(st.st_mode) ||
                access(conn->full_path.c_str(), R_OK) != 0)
            {
                response_code_set(conn, 404);
                response_print(__LINE__, __FUNCTION__, response_404, "%s can't open", conn->full_path.c_str());
                return false;
            }

            // 找后缀名
            const string &file_path = conn->file_path;
            string suffix;
            auto dot_pos = file_path.rfind('.');
            if (dot_pos != string::npos) {
                suffix = file_path.substr(dot_pos + 1);
            }
            cerr << suffix << endl;
            if (file_path == "favicon.ico") {
                conn->content_type = "image/x-icon";
            } else if (suffix == "png") {
                conn->content_type = "image/png";
            } else if (suffix == "jpg") {
                conn->content_type = "image/jpeg";
            } else if (suffix == "mp4") {
                conn->content_type = "video/mp4";
            } else {
                // never get here except .html
            }

        } else { // post
            if (allowed_post_path.find(conn->file_path) == allowed_post_path.end()) {
                response_code_set(conn, 400);
                response_print(__LINE__, __FUNCTION__, response_400, "%s isn't a legal post file path", conn->file_path.c_str());

                return false;
            }
        }

        conn->idx_to_check = conn->line_end + 2;
        return true;
    }
    static bool process_header(HttpConn *conn) {
        char *result = std::find(conn->idx_to_check, conn->line_end, ':'); // result is ':'
        if (result == conn->line_end || result == conn->line_end - 1) {
            response_code_set(conn, 400);
            response_print(__LINE__, __FUNCTION__, response_400, "");
            return false;
        }
        *result = '\0';

        // 处理认识而且需要的字段
        char *key = conn->idx_to_check;
        char *value = result + 1;
        while (value < conn->line_end && *value == ' ') ++value;
        if (value == conn->line_end) {
            response_code_set(conn, 400);
            response_print(__LINE__, __FUNCTION__, response_400, "");
            return false;
        }
        if (strcasecmp(key, "Cookie") == 0) {
            conn->cookie = value;
        } else if (strcasecmp(key, "Connection") == 0) {
            if (strcasecmp(value, "keep-alive") == 0) {
                conn->connection = "keep-alive";
            } else {
                response_code_set(conn, 400);
                response_print(__LINE__, __FUNCTION__, response_400, "connection: %s", value);
                return false;
            }
        } else if (strcasecmp(key, "Content-Length") == 0) {
            char *endp;
            errno = 0;
            long res = strtol(value, &endp, 10);
            if (errno != 0 || endp != conn->line_end || res < 0) {
                response_code_set(conn, 400);
                response_print(__LINE__, __FUNCTION__, response_400, "connection: %s", value);
                return false;
            }
            conn->post_content_length = res;
        }

        conn->idx_to_check = conn->line_end + 2;
        return true;
    }
    static void process_body(HttpConn *conn, bool &is_end, MYSQL *mysql_conn) {
        // alias, 要么const要么引用
        char *&idx_to_check = conn->idx_to_check;

        char *end_p = conn->idx_to_check + conn->post_content_length;
        int post_type;
        vector<const char*> keys; // allowed key list
        if (conn->file_path == "login") {
            keys = {"usr=", "passwd="};
            post_type = 1;
        } else if (conn->file_path == "signup") {
            keys = {"usr=", "passwd="};
            post_type = 2;
        } else { // post 的 file_path 的合法性在 parse_req_line 那里检验, 这个 else 就是摆设, 忘记写的时候的提醒
            err_print(__LINE__, __FUNCTION__, "code error, never get here");
            return;
        }
        const unsigned key_num = keys.size();
        vector<const char*> values(key_num);

        for (int i = 0; i < key_num; ++i) {
            // check key
            unsigned key_len = strlen(keys[i]);
            if (end_p - idx_to_check < key_len) {
                response_code_set(conn, 400);
                response_print(__LINE__, __FUNCTION__, response_400, "POST body key incorrect");
                return;
            }
            // 当然用 strncmp 了! idx_to_check 的最后又没有改成'\0', 一个字符串和另一个的一部分是否相等用 strncmp
            if (strncmp(keys[i], idx_to_check, key_len) != 0) {
                response_code_set(conn, 400);
                response_print(__LINE__, __FUNCTION__, response_400, "POST body key incorrect");
                return;
            }
            idx_to_check += key_len;

            // check and get value
            char *value_end = std::find(idx_to_check, end_p, '&');
            if (value_end == end_p && i != key_num - 1) {
                response_code_set(conn, 400);
                response_print(__LINE__, __FUNCTION__, response_400, "POST body key less than except");
                return;
            }
            if (value_end == idx_to_check) { // value 为空, 当作账号或密码错误吧
                response_code_set(conn, 302);
                conn->location = "/error.html";
                return;
            }

            *value_end = '\0';
            values[i] = idx_to_check;
            idx_to_check += (strlen(values[i]) + 1);
        }

        response_code_set(conn, 302);
        if (post_type == 1) { // login
            err_print(__LINE__, __FUNCTION__, "login ing");
            // SELECT passwd FROM user WHERE username = 'u' && passwd = 'p';
            string query_user = "SELECT passwd FROM users WHERE username = '";
            (query_user += values[0]) += "' && passwd = '";
            (query_user += values[1]) += "';";

            mysql_query(mysql_conn, query_user.c_str());
            MYSQL_RES *res = mysql_store_result(mysql_conn);
            if (mysql_num_rows(res) == 0) {
                conn->location = "/error.html";
//                return;
            } else {
                conn->location = "/welcome.html";
//                return;
            }

        } else if (post_type == 2) { // signup
            err_print(__LINE__, __FUNCTION__, "sign up ing");
            // INSERT INTO user (username, passwd) VALUES ('u', 'p');
            string add_user = "INSERT INTO users (username, passwd) VALUES ('";
            (add_user += values[0]) += "', '";
            (add_user += values[1]) += "');";

            int ret = mysql_query(mysql_conn, add_user.c_str());
            if (ret == 0) {
                conn->location = "/welcome.html";
//                return;
            } else {
                conn->location = "/error.html";
//                return;
            }
        }
    }
    static void response_code_set(HttpConn *conn, int code) {
        switch (code) {
        case 301:
            conn->response_code = response_301;
            conn->file_path = "error.html";
            break;
        case 302:
            conn->response_code = response_302;
            conn->file_path = "error.html";
            break;
        case 400:
            conn->response_code = response_400;
            conn->file_path = "400.html";
            break;
        case 403:
            conn->response_code = response_403;
            conn->file_path = "403.html";
            break;
        case 404:
            conn->response_code = response_404;
            conn->file_path = "404.html";
            break;
        default:
            // never get here
            break;
        }
        conn->full_path = conn->root_path + conn->file_path;
    }
    static void response(HttpConn *conn) {
        // 响应体包含实际要返回给客户端的内容,比如HTML文档、图片、JSON数据等
        int &file_fd = conn->file_fd;
        file_fd = open(conn->full_path.c_str(), O_RDONLY);
        if (file_fd == -1) {
            LOG_WARNING("open error");
            fprintf(stderr, "%d: open >>%s<< error\n", __LINE__, conn->full_path.c_str());
            printf("Error number: %d\n", errno);
            printf("Error message: %s\n", strerror(errno));
            exit(1);
        }
        struct stat sb;
        fstat(file_fd, &sb);
        conn->map_len = sb.st_size;
        cerr << "map_len set: " << conn->map_len << endl;
        conn->file_map = (char *) mmap(NULL, conn->map_len, PROT_READ, MAP_PRIVATE, file_fd, 0);
        if (conn->file_map == MAP_FAILED) {
            close(file_fd);
            return;
        }

        // 头部. 因为头部要用到body长度, 所以放后面
        fprintf(stderr, "%s: %d\n%s\n", __FUNCTION__, __LINE__, conn->response_code);
        char *&idx_to_write = conn->idx_to_write;
        idx_to_write += sprintf(idx_to_write, conn->response_code);
        idx_to_write += sprintf(idx_to_write, "Content-Type: %s\r\n", conn->content_type);
        idx_to_write += sprintf(idx_to_write, "Content-Length: %d\r\n", conn->map_len);
        if (!conn->connection.empty()) {
            idx_to_write += sprintf(idx_to_write, "Connection: %s\r\n", conn->connection.c_str());
        }
        if (!conn->set_cookie.empty()) {
            idx_to_write += sprintf(idx_to_write, "set-cookie: %s\r\n", conn->set_cookie.c_str());
        }
        if (!conn->location.empty()) {
            idx_to_write += sprintf(idx_to_write, "location: %s\r\n", conn->location.c_str());
        }
        idx_to_write += sprintf(idx_to_write, "\r\n");

        // 只要注册EPOLLOUT就能在收到EPOLLOUT, EPOLLOUT不是你想的那样
        Util::mod_fd(epfd, conn->connfd, EPOLLOUT, tuning.connfd_trig, true);
    }

    static const std::unordered_set<string> allowed_post_path;
};

const std::unordered_set<string> HttpProcessor::allowed_post_path = {"login", "signup"};


/***************** network *****************/

class Server : public NoCopy {
 public:
    Server() : port(tuning.port), connfd_trig(tuning.connfd_trig) {
        conns = new HttpConn[tuning.nofile_limit.rlim_cur];
    }
    void run() {

        // 新建
        int listenfd = socket(AF_INET, SOCK_STREAM, 0);
        if (listenfd < 0) {
            // error handle
            exit(1);
        }

        // 绑定
        struct sockaddr_in server_addr;
        server_addr.sin_family = AF_INET;
        server_addr.sin_addr.s_addr = INADDR_ANY;  // 绑定到任意可用的IP地址
        server_addr.sin_port = htons(port);
        if (bind(listenfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) != 0) {
            // error handle
            cerr << __LINE__ << endl;
            exit(1);
        }

        // 注册
        if (listen(listenfd, SOMAXCONN / 2) != 0) {
            LOG_ERROR("listen error");
            cerr << __LINE__ << endl;
            exit(1);
        }
        cerr << __LINE__ << endl;
        // 在epoll中注册listenfd
        epfd = epoll_create1(0);
        // epoll_create(int size)
        // Since Linux 2.6.8, the size argument is ignored, but must be > 0
        if (epfd == -1) {
            LOG_ERROR("epoll_create error");
            exit(1);
        }
        Util::add_fd(epfd, listenfd, EPOLLIN, LEVEL, false);
        struct epoll_event events[MAX_EVENTS]; // 放epoll_wait得到的的结果用

        while (true) {
            // epoll_wait timeout -1: block
            int event_cnt = epoll_wait(epfd, events, MAX_EVENTS, -1);
            cerr << "wait end\n";
            for (int i = 0; i < event_cnt; ++i) {
                const int eventfd = events[i].data.fd;
                if (eventfd == listenfd) { // 新连接
                    struct sockaddr_in client_addr = {0};
                    socklen_t client_addr_len = sizeof(client_addr);
                    int sockfd = accept(listenfd,
                                        (struct sockaddr *) &client_addr,
                                        &client_addr_len);
                    Util::set_noblock(sockfd);
                    Util::add_fd(epfd, sockfd, EPOLLIN, connfd_trig, true);
                    Util::init(conns + sockfd);
                    cerr << "new connection\n";

                } else if(events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                    Util::clean(conns + eventfd);
                    close(eventfd);
                    // 将来应该也不会有if之后的共通部分吧, 就不continue了

                } else if (events[i].events & EPOLLIN) { // 接收到了数据
                    cerr << "data in\n";
                    if (actor_model == PROACTOR) {
                        bool result = Util::recv(conns + eventfd);
                        if (!result) {
                            Util::clean(conns + eventfd);
                            close(eventfd);
                            continue;
                        }
                    }
                    conns[eventfd].connfd = eventfd;
                    conns[eventfd].mag = 12;

                    fprintf(stderr, "%s(%d): before run in thread\n", __FUNCTION__, __LINE__);
                    ThreadPool::get_instance().add_task({HttpProcessor::run,
                                                         conns + eventfd});

                } else if (events[i].events & EPOLLOUT) { // 要发回数据了
                    // 为什么不直接发送, 要用EPOLLOUT:
                    // 并发量大的话, 缓冲区肯定会满的啊, 满了总不能干等着吧
                    // 可以多次发送, 而且已经设置过非阻塞了
                    if (actor_model == PROACTOR) {
                        Util::deal_out(conns + eventfd, nullptr);
                    } else {
                        fprintf(stderr, "%s(%d): \n", __FUNCTION__, __LINE__);
                        ThreadPool::get_instance().add_task({Util::deal_out,
                                                             conns + eventfd});
                    }

                } else {
                    cerr << "never get\n";
                }
            }
        }
    }

 private:

    // 由 tuning 控制的常量
    int port;
    ActorModel actor_model;
    TrigMode connfd_trig;
    HttpConn* conns;
};


/* global function */

void Util::arg_parse(int argc, char *argv[]) {
    // -p <port> -s <so_linger(bool)> -t <thread_num> -q <sql_num>
    // -T <connfd_trig(0:L 1:E)> -A <actor_model(0:REACTOR 1:PROACTOR)>
    // -l <enable_log(bool)> -a <async_log(bool)> -v <log_lv(0-3)>
    int opt, tmp;
    const char *optstr = "p:s:t:q:T:A:l:a:v:";
    while ((opt = getopt(argc, argv, optstr)) != -1) {
        switch (tmp = atoi(optarg), opt) {
        case 'p':
            if (tmp > 0 && tmp < 10000) {
                tuning.port = tmp;
            } else {
                printf("argument %s of option -p illegal, using default instead\n", optarg, opt);
            }
            break;
        case 's':
            if (tmp == 0) {
                tuning.so_linger = false;
            } else if (tmp == 1) {
                tuning.so_linger = true;
            } else {
                printf("argument %s of option -%c is illegal, using default instead\n", optarg, opt);
            }
            break;
        case 't':
            if (tmp > 0) {
                tuning.thread_num = tmp;
            } else {
                printf("argument %s of option -%c is illegal, using default instead\n", optarg, opt);
            }
            break;
        case 'q':
            if (tmp > 0) {
                tuning.sql_num = tmp;
            } else {
                printf("argument %s of option -%c is illegal, using default instead\n", optarg, opt);
            }
            break;
        case 'T':
            if (tmp == 0 || tmp == 1) {
                tuning.connfd_trig = tmp == 1 ? EDGE : LEVEL;
            } else {
                printf("argument %s of option -%c is illegal, using default instead\n", optarg, opt);
            }
            break;
        case 'A':
            if (tmp == 0) {
                tuning.actor_model = REACTOR;
            } else if (tmp == 1) {
                tuning.actor_model = PROACTOR;
            } else {
                printf("argument %s of option -%c is illegal, using default instead\n", optarg, opt);
            }
            break;
        case 'l':
            if (tmp == 0) {
                tuning.enable_log = false;
            } else if (tmp == 1) {
                tuning.enable_log = true;
            } else {
                printf("argument %s of option -%c is illegal, using default instead\n", optarg, opt);
            }
            break;
        case 'a':
            if (tmp == 0) {
                tuning.async_log = false;
            } else if (tmp == 1) {
                tuning.async_log = true;
            } else {
                printf("argument %s of option -%c is illegal, using default instead\n", optarg, opt);
            }
            break;
        case 'v':
            if (tmp == 0) {
                tuning.log_lv = DEBUG;
            } else if (tmp == 1) {
                tuning.log_lv = INFO;
            } else if (tmp == 2) {
                tuning.log_lv = WARNING;
            } else if (tmp == 3) {
                tuning.log_lv = ERROR;
            } else {
                printf("argument %s of option -%c is illegal, using default instead\n", optarg, opt);
            }
            break;
        default:
            printf("option -%c is illegal, using default instead\n", opt);
            break;
        }
    }
}

int Util::set_noblock(int fd) {
    int old_flags = fcntl(fd, F_GETFL);
    if (old_flags == -1) {
        return -1;
    }
    int ret = fcntl(fd, F_SETFL, old_flags | O_NONBLOCK);
    if (ret == -1) {
        return -1;
    }
    return 0;
}

int Util::add_sig(int sig, void (*handler)(int), bool restart/* = false*/) {
    struct sigaction sa;
    sa.sa_handler = handler;
    if (restart) sa.sa_flags |= SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(sig, &sa, NULL);
    return 0;
}

// epoll_in_out 填 EPOLLIN / EPOLLOUT
int Util::add_fd(int epfd, int fd, int epoll_in_out, TrigMode trig_mode, bool oneshot) {
    struct epoll_event ev;
    ev.data.fd = fd;
    ev.events = epoll_in_out;
    if (trig_mode == EDGE) {
        ev.events |= EPOLLET;
    }
    if (oneshot) {
        ev.events |= EPOLLONESHOT;
    }
    epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
    return 0;
}

int Util::mod_fd(int epfd, int fd, int epoll_in_out, TrigMode trig_mode, bool oneshot) {
    struct epoll_event ev;
    ev.data.fd = fd;
    ev.events = epoll_in_out;
    if (trig_mode == EDGE) {
        ev.events |= EPOLLET;
    }
    if (oneshot) {
        ev.events |= EPOLLONESHOT;
    }
    epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
    return 0;
}

void Util::upscale_limit() {
    // 文件描述符上限调整
    setrlimit(RLIMIT_NOFILE, &tuning.nofile_limit);

    // nice调整
    setrlimit(RLIMIT_NICE, &tuning.nice_limit);

    // 设置全连接队列长度. 半连接队列由listen第二个参数设置, 我改成了SOMAXCONN / 2, 会不会内存不足影响性能?
    // 只改变宏的值没用的, 还要别的
#       define SOMAXCONN NEWSOMAXCONN
    FILE* fp = fopen("/proc/sys/net/core/somaxconn", "w");
    if(fp) {
        fprintf(fp, "%d", NEWSOMAXCONN);
        fclose(fp);
    } else {
        // error handler
    }
}

void Util::finish(int) {
    Log::get_instance().stop = true;
    Log::get_instance().notify.signal();
    ThreadPool::get_instance().stop = true;
    ThreadPool::get_instance().notify.broadcast();
}

MYSQL* Util::get_sql_conn() {
    MYSQL *conn = mysql_init(NULL);
    if (!conn) {
        LOG_ERROR("mysql init error: %s", mysql_errno(conn));
        exit(1);
    }
    auto ret = mysql_real_connect(conn, mysql_ip, user,
                                  passwd, db_name, MYSQL_PORT,
                                  NULL, 0);
    if (ret == NULL) {
        fprintf(stderr, "mysql connect error, do you use sudo?");
        LOG_ERROR("mysql connect error: %s", mysql_error(conn));
        mysql_close(conn);
        exit(1);
    }
    return conn;
}

int Util::return_sql_conn(MYSQL *conn) {
    mysql_close(conn);
}

void Util::init(HttpConn *http_conn) {
    cerr << "init called\n";
    http_conn->idx_to_check = http_conn->rec_buf;
    http_conn->rec_idx = http_conn->rec_buf;
    http_conn->idx_to_write = http_conn->send_buf;
    http_conn->send_idx = http_conn->send_buf;
    http_conn->line_num_parsed = 0;
    http_conn->empty_line = false;
    http_conn->line_status = OPEN;
    http_conn->connection.clear();
    http_conn->cookie.clear();
    http_conn->post_content_length = 0;
    http_conn->content_type = "text/html; charset=utf-8";
    http_conn->set_cookie.clear();
    http_conn->location.clear();
    http_conn->sent_len = 0;
    http_conn->mag = 5;
}

bool Util::recv(HttpConn *http_conn) {
    using ::recv;
    int connfd = http_conn->connfd;
    char *&rec_idx = http_conn->rec_idx;
    if (tuning.connfd_trig == EDGE) { // edge trig mode
        while (true) {
            int ret = recv(connfd, rec_idx,
                           REC_BUF_LEN - 1 - (rec_idx - http_conn->rec_buf), 0);
            // 一次连接一个缓冲区. 缓冲区不够大就返回-1
            if (ret == -1 &&
                (errno == EAGAIN || errno == EWOULDBLOCK)) {
                // oneshot 要重置的, 在撒手不管但是还没结束(close)的时候, 也就是 EAGAIN 的时候
                mod_fd(epfd, connfd, EPOLLIN, tuning.connfd_trig, true);
                break;
            } else if (ret == 0 || ret == -1) {
                return false;
            }
            rec_idx += ret; // rec_idx 是一个引用所以可以
            if (rec_idx >= http_conn->rec_buf + REC_BUF_LEN) {
                return false;
            }
        }

    } else { // level trig mode
        int ret = recv(connfd, rec_idx,
                       REC_BUF_LEN - 1 - (rec_idx - http_conn->rec_buf), 0);
        if (ret == 0 || ret == -1) {
            return false;
        }
        rec_idx += ret;
        if (rec_idx >= http_conn->rec_buf + REC_BUF_LEN) {
            return false;
        }
    }
    return true;
}

void Util::deal_out(HttpConn *conn, MYSQL*) {
    fprintf(stderr, "%s: %d\n", __FUNCTION__, __LINE__);
    if (!send(conn)) {
        clean(conn);
        close(conn->connfd);
        return;
    }
    if (conn->sent_len == conn->map_len) {
        clean(conn);
        if (conn->connection.empty()) { // isn't keep-alive
            close(conn->connfd);
        } else { // keep-alive省掉了连接, 必须把listenfd那里的步骤全部补上
            Util::mod_fd(epfd, conn->connfd, EPOLLIN, tuning.connfd_trig, true);
            init(conn);
        }
    }
}

bool Util::send(HttpConn *conn) {
    using ::send;

    const int connfd = conn->connfd;
    char *&send_idx = conn->send_idx;
    char * const idx_to_write = conn->idx_to_write;
    while (send_idx < idx_to_write) {
        int ret = send(connfd, send_idx, idx_to_write - send_idx, 0);

        if (ret == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            mod_fd(epfd, connfd, EPOLLOUT, tuning.connfd_trig, true);
            return true;
        } else if (ret == 0 || ret == -1) {
            return false;
        }
        send_idx += ret; // send_idx 是个引用
    }
    fprintf(stderr, "%s: %d\n", __FUNCTION__, __LINE__);

    int &sent_len = conn->sent_len;
    const int map_len = conn->map_len;
    fprintf(stderr, "%s(%d): sent_len: %d, map_len: %d\n", __FUNCTION__, __LINE__, conn->sent_len, conn->map_len);
    while (sent_len < map_len) {
        fprintf(stderr, "%s(%d): sent_len: %d, map_len: %d\n", __FUNCTION__, __LINE__, conn->sent_len, conn->map_len);
        int ret = send(connfd, conn->file_map + sent_len, map_len - sent_len, 0);

        if (ret == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            mod_fd(epfd, connfd, EPOLLOUT, tuning.connfd_trig, true);
            return true;
        } else if (ret == 0 || ret == -1) {
            return false;
        }
        sent_len += ret; // send_idx 是个引用
    }
    fprintf(stderr, "%s: %d\n", __FUNCTION__, __LINE__);

    return true;
}

void Util::clean(HttpConn *conn) {
    munmap(conn->file_map, conn->map_len);
    close(conn->file_fd);
}

int main(int argc, char *argv[]) {
    // preparing
    Util::arg_parse(argc, argv);
    Util::upscale_limit();
    Util::add_sig(SIGINT, Util::finish);
    Util::add_sig(SIGPIPE, SIG_IGN);

    cerr << __LINE__ << endl;
    Server().run();

    return 0;
}