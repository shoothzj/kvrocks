#include <fcntl.h>
#include <string.h>
#include <strings.h>
#include <glog/logging.h>

#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

#include "config.h"
#include "string_util.h"
#include "status.h"
#include "cron.h"

static const std::vector<std::string> loglevels {"info", "warning", "error", "fatal"};
static const char *default_namespace = "__namespace";

void Config::incrOpenFilesLimit(rlim_t maxfiles) {
  struct rlimit limit;

  rlim_t old_limit, best_limit = maxfiles, decr_step = 16;
  if (getrlimit(RLIMIT_NOFILE, &limit) < 0 || best_limit <= limit.rlim_cur) {
    return;
  }
  old_limit = limit.rlim_cur;
  while(best_limit > old_limit) {
    limit.rlim_cur = best_limit;
    limit.rlim_max = best_limit;
    if (setrlimit(RLIMIT_NOFILE,&limit) != -1) break;
    /* We failed to set file limit to 'bestlimit'. Try with a
     * smaller limit decrementing by a few FDs per iteration. */
    if (best_limit < decr_step) break;
    best_limit -= decr_step;
  }
}

int Config::yesnotoi(std::string input) {
  if (strcasecmp(input.data(), "yes") == 0) {
    return 1;
  } else if (strcasecmp(input.data(), "no") == 0) {
    return 0;
  }
  return -1;
}

bool Config::parseRocksdbOption(std::string key, std::string value, std::string *err) {
  int32_t n;
  try {
    n = std::stoi(value);
  } catch (std::exception &e) {
    *err = e.what();
    return false;
  }
  if (key == "max_open_files" ) {
    rocksdb_options.max_open_files = n;
  } else if (!strncasecmp(key.data(), "write_buffer_size" , strlen("write_buffer_size"))) {
    if (n < 16 || n > 4096) {
      *err = "write_buffer_size should be between 16MB and 4GB";
      return false;
    }
    rocksdb_options.write_buffer_size = static_cast<size_t>(n) * 1048576;
  }  else if (key == "max_write_buffer_number" ) {
    if (n < 1 || n > 64) {
      *err = "max_write_buffer_number should be between 1 and 64";
      return false;
    }
    rocksdb_options.max_write_buffer_number = n;
  }  else if (key == "max_background_compactions" ) {
    if (n < 1 || n > 16) {
      *err = "max_background_compactions should be between 1 and 16";
      return false;
    }
    rocksdb_options.max_background_compactions = n;
  }  else if (key == "max_background_flushes" ) {
    if (n < 1 || n > 16) {
      *err = "max_background_flushes should be between 1 and 16";
      return false;
    }
    rocksdb_options.max_background_flushes = n;
  }  else if (key == "max_sub_compactions" ) {
    if (n < 1 || n > 8) {
      *err = "max_sub_compactions should be between 1 and 8";
      return false;
    }
    rocksdb_options.max_sub_compactions = static_cast<uint32_t>(n);
  } else {
    *err = "Bad directive or wrong number of arguments";
    return false;
  }
  return true;
}

bool Config::parseConfigFromString(std::string input, std::string *err) {
  std::vector<std::string> args;
  Util::Split(input, " \t\r\n", &args);
  // omit empty line and comment
  if (args.empty() || args[0].front() == '#') return true;

  size_t size = args.size();
  if (size == 2 && args[0] == "port") {
    port = std::stoi(args[1]);
    repl_port = port + 1;
  } else if (size == 2 && args[0] == "timeout") {
    timeout = std::stoi(args[1]);
  } else if (size == 2 && args[0] == "workers") {
    workers = std::stoi(args[1]);
    if (workers < 1 || workers > 1024) {
      *err = "too many worker threads";
      return false;
    }
  } else if (size == 2 && args[0] == "repl-workers" ){
    repl_workers = std::stoi(args[1]);
    if (workers < 1 || workers > 1024) {
      *err = "too many replication worker threads";
      return false;
    }
  } else if (size >= 2 && args[0] == "bind") {
    binds.clear();
    for (unsigned i = 1; i < args.size(); i++) {
      binds.emplace_back(args[i]);
    }
  } else if (size >= 2 && args[0] == "repl-bind") {
    repl_binds.clear();
    for (unsigned i = 1; i < args.size(); i++) {
      repl_binds.emplace_back(args[i]);
    }
  }else if (size == 2 && args[0] == "daemonize") {
    int i;
    if ((i = yesnotoi(args[1])) == -1) {
      *err = "argument must be 'yes' or 'no'";
      return false;
    }
    daemonize = (i == 1);
  } else if (size == 2 && args[0] == "slave-read-only") {
    int i;
    if ((i = yesnotoi(args[1])) == -1) {
      *err = "argument must be 'yes' or 'no'";
      return false;
    }
    slave_readonly = (i == 1);
  } else if (size == 2 && args[0] == "tcp-backlog") {
    backlog = std::stoi(args[1]);
  } else if (size == 2 && args[0] == "dir") {
    db_dir = args[1];
    if (db_dir.back() != '/') {
      backup_dir = db_dir + "/backup";
    } else {
      backup_dir = db_dir + "backup";
    }
  } else if (size == 2 && args[0] == "maxclients") {
    maxclients = std::stoi(args[1]);
    if (maxclients > 0) incrOpenFilesLimit(static_cast<rlim_t >(maxclients));
  } else if (size == 2 && args[0] == "db-name") {
    db_name = args[1];
  } else if (size == 2 && args[0] == "masterauth") {
    master_auth = args[1];
  } else if (size == 2 && args[0] == "requirepass") {
    require_passwd = args[1];
  } else if (size == 2 && args[0] == "pidfile") {
    pidfile = args[1];
  } else if (size == 2 && args[0] == "loglevel") {
    for (size_t i = 0; i < loglevels.size(); i++) {
      if (Util::ToLower(args[1]) == loglevels[i]) {
        loglevel = static_cast<int>(i);
        break;
      }
    }
  } else if (size == 3 && args[0] == "slaveof") {
    if (args[1] != "no" && args[2] != "one") {
      master_host = args[1];
      // we use port + 1 as repl port, so incr the slaveof port here
      master_port = std::stoi(args[2]) + 1;
      if (master_port <= 0 || master_port >= 65535) {
        *err = "master port range should be between 0 and 65535";
        return false;
      }
    }
  } else if (size == 6 && args[0] == "compact-cron") {
    Status s = compact_cron.SetParams(args[1], args[2], args[3], args[4], args[5]);
    if (!s.IsOK()) {
      *err = "compact-cron time expression format error : " + s.Msg();
      return false;
    }
  } else if (size == 6 && args[0] == "bgsave-cron") {
    Status s = bgsave_cron.SetParams(args[1], args[2], args[3], args[4], args[5]);
    if (!s.IsOK()) {
      *err = "bgsave-cron time expression format error : " + s.Msg();
      return false;
    }
  } else if (size == 2 && !strncasecmp(args[0].data(), "rocksdb.", 8)) {
    return parseRocksdbOption(args[0].substr(8, args[0].size() - 8), args[1], err);
  } else if (size == 2 && !strncasecmp(args[0].data(), "namespace.", 10)) {
    std::string ns = args[0].substr(10, args.size()-10);
    if(ns.size() > INT8_MAX) {
      *err = std::string("namespace size exceed limit ")+std::to_string(INT8_MAX);
      return false;
    }
    tokens[args[1]] = ns;
  } else if (size == 2 && !strcasecmp(args[0].data(), "slowlog-log-slower-than")) {
    slowlog_log_slower_than = std::stoll(args[1]);
  } else if (size == 2 && !strcasecmp(args[0].data(), "slowlog-max-len")) {
    slowlog_max_len = std::stoi(args[1]);
  } else {
    *err = "Bad directive or wrong number of arguments";
    return false;
  }
  return true;
}

bool Config::Load(std::string path, std::string *err) {
  path_ = std::move(path);
  std::ifstream file(path_);
  if (!file.is_open()) {
    *err = strerror(errno);
    return false;
  }

  std::string line, parse_err;
  int line_num = 1;
  while (!file.eof()) {
    std::getline(file, line);
    line = Util::ToLower(line);
    if (!parseConfigFromString(line, &parse_err)) {
      *err = std::string("failed to parse config at line: #L")
          + std::to_string(line_num) + ", err:" + parse_err;
      file.close();
      return false;
    }
    line_num++;
  }
  if (require_passwd.empty()) {
    *err = "requirepass cannot be empty";
    return false;
  }
  tokens[require_passwd] = default_namespace;
  file.close();
  return true;
}

bool Config::rewriteConfigValue(std::vector<std::string> &args) {
  size_t size = args.size();

  if (size == 2 && args[0] == "timeout") {
    if (std::to_string(timeout) != args[1]) {
      args[1] = std::to_string(timeout);
      return true;
    }
  } else if (size == 2 && args[0] == "maxclients") {
    if (std::to_string(maxclients) != args[1]) {
      args[1] = std::to_string(maxclients);
      return true;
    }
  } else if (size == 2 && args[0] == "masterauth") {
    if (master_auth!= args[1]) {
      args[1] = master_auth;
      return true;
    }
  } else if (size == 2 && args[0] == "requirepass") {
    if (require_passwd != args[1]) {
      args[1] = require_passwd;
      return true;
    }
  } else if (size == 2 && args[0] == "slave-read-only") {
    args[1] = slave_readonly? "yes":"no";
    return true;
  } else if (size == 2 && args[0] == "loglevel") {
    if (args[1] != loglevels[loglevel]) {
      args[1] = loglevels[loglevel];
    }
  }
  return false;
}

void Config::Get(std::string &key, std::vector<std::string> *values) {
  key = Util::ToLower(key);
  values->clear();
  bool is_all = key == "*", is_rocksdb_all = key == "rocksdb.*";
  if (is_all || key == "port") {
    values->emplace_back("port");
    values->emplace_back(std::to_string(port));
  }
  if (is_all || key == "workers") {
    values->emplace_back("workers");
    values->emplace_back(std::to_string(workers));
  }
  if (is_all || key == "timeout") {
    values->emplace_back("timeout");
    values->emplace_back(std::to_string(timeout));
  }
  if (is_all || key == "loglevel"){
    values->emplace_back("loglevel");
    values->emplace_back(loglevels[loglevel]);
  }
  if (is_all || key == "tcp-backlog") {
    values->emplace_back("tcp-backlog");
    values->emplace_back(std::to_string(backlog));
  }
  if (is_all || key == "maxclients") {
    values->emplace_back("maxclients");
    values->emplace_back(std::to_string(maxclients));
  }
  if (is_all || key == "daemonize") {
    values->emplace_back("daemonize");
    values->emplace_back(daemonize ? "yes" : "no");
  }
  if (is_all || key == "slave-read-only") {
    values->emplace_back("slave-read-only");
    values->emplace_back(slave_readonly? "yes" : "no");
  }
  if (is_all || key == "pidfile") {
    values->emplace_back("pidfile");
    values->emplace_back(pidfile);
  }
  if (is_all || key == "db-name") {
    values->emplace_back("db-name");
    values->emplace_back(db_name);
  }
  if (is_all || key == "dir") {
    values->emplace_back("dir");
    values->emplace_back(db_dir);
  }
  if (is_all || key == "masterauth") {
    values->emplace_back("masterauth");
    values->emplace_back(master_auth);
  }
  if (is_all || key == "requirepass") {
    values->emplace_back("requirepass");
    values->emplace_back(require_passwd);
  }
  if (is_all || key == "slaveof") {
    values->emplace_back("slaveof");
    if (master_host.empty()) {
      values->emplace_back("");
    } else {
      values->emplace_back(master_host+" "+ std::to_string(master_port));
    }
  }
  if (is_all || key == "binds") {
    std::string binds_str;
    for (const auto &bind : binds) {
      binds_str.append(bind);
      binds_str.append(",");
    }
    binds_str = binds_str.substr(0, binds_str.size()-1);
    values->emplace_back("binds");
    values->emplace_back(binds_str);
  }
  if (is_all || key == "compact-cron") {
    values->emplace_back("compact-cron");
    if (!compact_cron.IsEnabled()) {
      values->emplace_back("");
    } else {
      values->emplace_back(compact_cron.ToString());
    }
  }
  if (is_all || key == "bgsave-cron") {
    values->emplace_back("bgsave-cron");
    if (!bgsave_cron.IsEnabled()) {
      values->emplace_back("");
    } else {
      values->emplace_back(bgsave_cron.ToString());
    }
  }
  if (is_rocksdb_all || key == "rocksdb.max_open_files") {
    values->emplace_back("rocksdb.max_open_files");
    values->emplace_back(std::to_string(rocksdb_options.max_open_files));
  }
  if (is_rocksdb_all || key == "rocksdb.write_buffer_size") {
    values->emplace_back("rocksdb.write_buffer_size");
    values->emplace_back(std::to_string(rocksdb_options.write_buffer_size));
  }
  if (is_rocksdb_all || key == "rocksdb.block_cache_size") {
    values->emplace_back("rocksdb.block_cache_size");
    values->emplace_back(std::to_string(rocksdb_options.block_cache_size));
  }
  if (is_rocksdb_all || key == "rocksdb.max_write_buffer_number") {
    values->emplace_back("rocksdb.max_write_buffer_number");
    values->emplace_back(std::to_string(rocksdb_options.max_write_buffer_number));
  }
  if (is_rocksdb_all || key == "rocksdb.max_background_compactions") {
    values->emplace_back("rocksdb.max_background_compactions");
    values->emplace_back(std::to_string(rocksdb_options.max_background_compactions));
  }
  if (is_rocksdb_all || key == "rocksdb.max_background_flushes") {
    values->emplace_back("rocksdb.max_background_flushes");
    values->emplace_back(std::to_string(rocksdb_options.max_background_flushes));
  }
  if (is_rocksdb_all || key == "rocksdb.max_sub_compactions") {
    values->emplace_back("rocksdb.max_sub_compactions");
    values->emplace_back(std::to_string(rocksdb_options.max_sub_compactions));
  }
}

Status Config::Set(std::string &key, std::string &value) {
  key = Util::ToLower(key);
  if (key == "timeout") {
    timeout = std::stoi(value);
    return Status::OK();
  }
  if (key == "maxclients") {
    timeout = std::stoi(value);
    return Status::OK();
  }
  if (key == "masterauth") {
    master_auth = value;
    return Status::OK();
  }
  if (key == "requirepass") {
    if (value.empty()) {
      return Status(Status::NotOK, "requirepass cannot be empty");
    }
    tokens.erase(require_passwd);
    require_passwd = value;
    tokens[require_passwd] = default_namespace;
    LOG(WARNING) << "Updated requirepass,  new requirepass: " << value;
    return Status::OK();
  }
  if (key == "slave-read-only") {
    int i;
    if ((i = yesnotoi(value)) == -1) {
      return Status(Status::NotOK, "argument must be 'yes' or 'no'");
    }
    slave_readonly = (i == 1);
    return Status::OK();
  }
  if (key == "loglevel") {
    for (size_t i = 0; i < loglevels.size(); i++) {
      if (Util::ToLower(value) == loglevels[i]) {
        loglevel = static_cast<int>(i);
        break;
      }
    }
    return Status(Status::NotOK, "loglevel should be info,warning,error,fatal");
  }
  if (key == "compact-cron") {
    std::vector<std::string> args;
    Util::Split(value, " ", &args);
    if (args.empty()) {
      compact_cron.Disable();
      return Status::OK();
    }
    if (args.size() != 5) {
      return Status(Status::NotOK, "time expression format error,should include 5 field");
    }
    Status s = compact_cron.SetParams(args[0], args[1], args[2], args[3], args[4]);
    if (!s.IsOK()) {
      return Status(Status::NotOK, "time expression format error : " + s.Msg());
    }
    return Status::OK();
  }
  if (key == "bgsave-cron") {
    std::vector<std::string> args;
    Util::Split(value, " ", &args);
    if (args.empty()) {
      bgsave_cron.Disable();
      return Status::OK();
    }
    if (args.size() != 5) {
      return Status(Status::NotOK, "time expression format error,should include 5 field");
    }
    Status s = bgsave_cron.SetParams(args[0], args[1], args[2], args[3], args[4]);
    if (!s.IsOK()) {
      return Status(Status::NotOK, "time expression format error : " + s.Msg());
    }
    return Status::OK();
  }
  return Status(Status::NotOK, "Unsupported CONFIG parameter");
}

bool Config::Rewrite(std::string *err) {
  std::string tmp_path = path_+".tmp";
  std::ostringstream string_stream;

  remove(tmp_path.data());
  std::ifstream input_file(path_, std::ios::in);
  std::ofstream output_file(tmp_path, std::ios::out);
  if (!input_file.is_open() || !output_file.is_open()) {
    if (input_file.is_open()) input_file.close();
    *err = strerror(errno);
    return false;
  }

  std::string line, new_line, buffer;
  std::vector<std::string> args;
  while (!input_file.eof()) {
    std::getline(input_file, line);
    Util::Split(line, " \t\r\n", &args);
    if (args.empty() || args[0].front() == '#'
        || !rewriteConfigValue(args)) {
      if (!strncasecmp(args[0].data(), "namespace.", 10)) {
        // skip the namespace, append at the end
        continue;
      }
      buffer.append(line);
      buffer.append("\n");
    } else {
      string_stream.str(std::string());
      string_stream.clear();
      for (const auto &arg : args) {
        string_stream << arg << " ";
      }
      buffer.append(string_stream.str());
      buffer.append("\n");
    }
  }

  string_stream.str(std::string());
  string_stream.clear();
  for (auto iter = tokens.begin(); iter != tokens.end(); ++iter) {
    if (iter->first != require_passwd) {
      string_stream << "namespace." << iter->second << " " << iter->first << "\n";
    }
  }
  buffer.append(string_stream.str());

  output_file.write(buffer.data(), buffer.size());
  input_file.close();
  output_file.close();
  if (rename(tmp_path.data(), path_.data()) < 0) {
    *err = "failed to swap the config file, err: "+ std::string(strerror(errno));
    return false;
  }
  return true;
}

void Config::GetNamespace(std::string &ns, std::string *token) {
  for (auto iter = tokens.begin(); iter != tokens.end(); iter++) {
    if (iter->second == ns) {
      *token = iter->first;
    }
  }
}

Status Config::SetNamepsace(std::string &ns, std::string token) {
  if (ns == default_namespace) {
    return Status(Status::NotOK, "can't set the default namespace");
  }
  if (tokens.find(token) != tokens.end()) {
    return Status(Status::NotOK, "the token has already exists");
  }
  for (auto iter = tokens.begin(); iter != tokens.end(); iter++) {
    if (iter->second == ns) {
      tokens.erase(iter);
      tokens[token] = ns;
      LOG(WARNING) << "Updated namespace: " << ns << ", new token: " << token;
      return Status::OK();
    }
  }
  return Status(Status::NotOK, "namespace was not found");
}

Status Config::AddNamespace(const std::string &ns, const std::string &token) {
  if (ns.size() > 255) {
    return Status(Status::NotOK, "namespace size exceed limit " + std::to_string(INT8_MAX));
  }
  if (tokens.find(token) != tokens.end()) {
    return Status(Status::NotOK, "the token has already exists");
  }
  for (auto iter = tokens.begin(); iter != tokens.end(); iter++) {
    if (iter->second == ns) {
      return Status(Status::NotOK, "namespace has already exists");
    }
  }
  tokens[token] = ns;
  LOG(WARNING) << "Create new namespace: " << ns << ", token: " << token;
  return Status::OK();
}

Status Config::DelNamespace(std::string &ns) {
  if (ns == default_namespace) {
    return Status(Status::NotOK, "can't del the default namespace");
  }
  for (auto iter = tokens.begin(); iter != tokens.end(); iter++) {
    if (iter->second == ns) {
      tokens.erase(iter);
      LOG(WARNING) << "Deleted namespace: " << ns << ", token: " << iter->first;
      return Status::OK();
    }
  }
  return Status(Status::NotOK, "namespace was not found");
}
