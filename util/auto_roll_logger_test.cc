// Copyright (c) 2012 Facebook. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <string>
#include <cmath>
#include "util/testharness.h"
#include "util/auto_roll_logger.h"
#include "leveldb/db.h"
#include <sys/stat.h>
#include <errno.h>
#include <iostream>

using namespace std;

namespace leveldb {

// annoymous namespace for test facilities.
namespace {

// This is a fake logger that keeps counting
// the size of logged messages.
class MockLogger: public Logger {
 public:
  MockLogger(): log_size_(0) { }
  // In our simple cases, we only use `format` parameter.
  void Logv(const char* format, va_list ap) {
    log_size_ += strlen(format);
  }
  virtual size_t GetLogFileSize() const {
    return log_size_;
  }

 private:
  size_t log_size_;
};

// A fake Env class that can create MockLogger instance.
class MockEnv: public EnvWrapper {
 public:
  static Env* MakeMockEnv() {
    Env* target = Env::Default();
    return new MockEnv(target);
  }

  ~MockEnv() {
  }

  virtual Status NewLogger(const std::string& fname, shared_ptr<Logger>* result) {
    result->reset(new MockLogger());
    return Status::OK();
  }

  virtual Status RenameFile(const std::string& src,
                            const std::string& target) {
    // do nothing
    return Status::OK();
  }

 private:
  MockEnv(Env* target): EnvWrapper(target), target_(target) { }
  Env* target_;
};

} // end anonymous namespace

class AutoRollLoggerTest {
 public:
  static void InitTestDb() {
    string deleteCmd = "rm -rf " + kTestDir;
    system(deleteCmd.c_str());
    Env::Default()->CreateDir(kTestDir);
  }

  void RollLogFileBySizeTest(AutoRollLogger* logger,
                             size_t log_max_size,
                             const string& log_message);
  uint64_t RollLogFileByTimeTest(AutoRollLogger* logger,
                                 size_t time,
                                 const string& log_message);

  static const string kSampleMessage;
  static const string kTestDir;
  static const string kLogFile;
  static Env* env;
  static Env* mockEnv;
};

const string AutoRollLoggerTest::kSampleMessage(
    "this is the message to be written to the log file!!");
const string AutoRollLoggerTest::kTestDir(
    test::TmpDir() + "/db_log_test");
const string AutoRollLoggerTest::kLogFile(
    test::TmpDir() + "/db_log_test/LOG");
Env* AutoRollLoggerTest::mockEnv = MockEnv::MakeMockEnv();
Env* AutoRollLoggerTest::env = Env::Default();

void GetFileCreateTime(const std::string& fname, uint64_t* file_ctime) {
  struct stat s;
  if (stat(fname.c_str(), &s) != 0) {
    *file_ctime = (uint64_t)0;
  }
  *file_ctime = static_cast<uint64_t>(s.st_ctime);
}

void AutoRollLoggerTest::RollLogFileBySizeTest(AutoRollLogger* logger,
                                               size_t log_max_size,
                                               const string& log_message) {
  // measure the size of each message, which is supposed
  // to be equal or greater than log_message.size()
  Log(logger, log_message.c_str());
  size_t message_size = logger->GetLogFileSize();
  size_t current_log_size = message_size;

  // Test the cases when the log file will not be rolled.
  while (current_log_size + message_size < log_max_size) {
    Log(logger, log_message.c_str());
    current_log_size += message_size;
    ASSERT_EQ(current_log_size, logger->GetLogFileSize());
  }

  // Now the log file will be rolled
  Log(logger, log_message.c_str());
  ASSERT_TRUE(0 == logger->GetLogFileSize());
}

uint64_t AutoRollLoggerTest::RollLogFileByTimeTest(
    AutoRollLogger* logger, size_t time, const string& log_message) {
  uint64_t expected_create_time;
  uint64_t actual_create_time;
  uint64_t total_log_size;
  ASSERT_OK(env->GetFileSize(kLogFile, &total_log_size));
  GetFileCreateTime(kLogFile, &expected_create_time);
  logger->SetCallNowMicrosEveryNRecords(0);

  // -- Write to the log for several times, which is supposed
  // to be finished before time.
  for (int i = 0; i < 10; ++i) {
     Log(logger, log_message.c_str());
     ASSERT_OK(logger->GetStatus());
     // Make sure we always write to the same log file (by
     // checking the create time);
     GetFileCreateTime(kLogFile, &actual_create_time);

     // Also make sure the log size is increasing.
     ASSERT_EQ(expected_create_time, actual_create_time);
     ASSERT_GT(logger->GetLogFileSize(), total_log_size);
     total_log_size = logger->GetLogFileSize();
  }

  // -- Make the log file expire
  sleep(time);
  Log(logger, log_message.c_str());

  // At this time, the new log file should be created.
  GetFileCreateTime(kLogFile, &actual_create_time);
  ASSERT_GT(actual_create_time, expected_create_time);
  ASSERT_LT(logger->GetLogFileSize(), total_log_size);
  expected_create_time = actual_create_time;

  return expected_create_time;
}

TEST(AutoRollLoggerTest, RollLogFileBySize) {
    size_t log_max_size = 1024;

    AutoRollLogger* logger = new AutoRollLogger(
        mockEnv, kTestDir, "", log_max_size, 0);

    RollLogFileBySizeTest(logger, log_max_size,
                          kSampleMessage + ":RollLogFileBySize");

    delete logger;
}

TEST(AutoRollLoggerTest, RollLogFileByTime) {
    size_t time = 1;
    size_t log_size = 1024 * 5;

    InitTestDb();
    // -- Test the existence of file during the server restart.
    ASSERT_TRUE(!env->FileExists(kLogFile));
    AutoRollLogger* logger = new AutoRollLogger(
        Env::Default(), kTestDir, "", log_size, 1);
    ASSERT_TRUE(env->FileExists(kLogFile));

    RollLogFileByTimeTest(logger, time, kSampleMessage + ":RollLogFileByTime");

    delete logger;
}

TEST(AutoRollLoggerTest,
     OpenLogFilesMultipleTimesWithOptionLog_max_size) {
  // If only 'log_max_size' options is specified, then every time
  // when leveldb is restarted, a new empty log file will be created.
  InitTestDb();
  // WORKAROUND:
  // avoid complier's complaint of "comparison between signed
  // and unsigned integer expressions" because literal 0 is
  // treated as "singed".
  size_t kZero = 0;
  size_t log_size = 1024;

  AutoRollLogger* logger = new AutoRollLogger(
    Env::Default(), kTestDir, "", log_size, 0);

  Log(logger, kSampleMessage.c_str());
  ASSERT_GT(logger->GetLogFileSize(), kZero);
  delete logger;

  // reopens the log file and an empty log file will be created.
  logger = new AutoRollLogger(
    Env::Default(), kTestDir, "", log_size, 0);
  ASSERT_EQ(logger->GetLogFileSize(), kZero);
  delete logger;
}

TEST(AutoRollLoggerTest, CompositeRollByTimeAndSizeLogger) {
  size_t time = 1, log_max_size = 1024 * 5;

  InitTestDb();

  AutoRollLogger* logger = new AutoRollLogger(
        Env::Default(), kTestDir, "", log_max_size, time);

  // Test the ability to roll by size
  RollLogFileBySizeTest(
      logger, log_max_size,
      kSampleMessage + ":CompositeRollByTimeAndSizeLogger");

  // Test the ability to roll by Time
  RollLogFileByTimeTest( logger, time,
      kSampleMessage + ":CompositeRollByTimeAndSizeLogger");
}

TEST(AutoRollLoggerTest, CreateLoggerFromOptions) {
  Options options;
  shared_ptr<Logger> logger;

  // Normal logger
  ASSERT_OK(CreateLoggerFromOptions(kTestDir, "", env, options, &logger));
  ASSERT_TRUE(dynamic_cast<PosixLogger*>(logger.get()) != NULL);

  // Only roll by size
  InitTestDb();
  options.max_log_file_size = 1024;
  ASSERT_OK(CreateLoggerFromOptions(kTestDir, "", env, options, &logger));
  AutoRollLogger* auto_roll_logger =
    dynamic_cast<AutoRollLogger*>(logger.get());
  ASSERT_TRUE(auto_roll_logger != NULL);
  RollLogFileBySizeTest(
      auto_roll_logger, options.max_log_file_size,
      kSampleMessage + ":CreateLoggerFromOptions - size");

  // Only roll by Time
  InitTestDb();
  options.max_log_file_size = 0;
  options.log_file_time_to_roll = 1;
  ASSERT_OK(CreateLoggerFromOptions(kTestDir, "", env, options, &logger));
  auto_roll_logger =
    dynamic_cast<AutoRollLogger*>(logger.get());
  RollLogFileByTimeTest(
      auto_roll_logger, options.log_file_time_to_roll,
      kSampleMessage + ":CreateLoggerFromOptions - time");

  // roll by both Time and size
  InitTestDb();
  options.max_log_file_size = 1024 * 5;
  options.log_file_time_to_roll = 1;
  ASSERT_OK(CreateLoggerFromOptions(kTestDir, "", env, options, &logger));
  auto_roll_logger =
    dynamic_cast<AutoRollLogger*>(logger.get());
  RollLogFileBySizeTest(
      auto_roll_logger, options.max_log_file_size,
      kSampleMessage + ":CreateLoggerFromOptions - both");
  RollLogFileByTimeTest(
      auto_roll_logger, options.log_file_time_to_roll,
      kSampleMessage + ":CreateLoggerFromOptions - both");
}

int OldLogFileCount(const string& dir) {
  std::vector<std::string> files;
  Env::Default()->GetChildren(dir, &files);
  int log_file_count = 0;

  for (std::vector<std::string>::iterator it = files.begin();
       it != files.end(); ++it) {
    uint64_t create_time;
    FileType type;
    if (!ParseFileName(*it, &create_time, &type)) {
      continue;
    }
    if (type == kInfoLogFile && create_time > 0) {
      ++log_file_count;
    }
  }

  return log_file_count;
}

}  // namespace leveldb

int main(int argc, char** argv) {
  return leveldb::test::RunAllTests();
}
