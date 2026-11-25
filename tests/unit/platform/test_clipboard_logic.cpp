/**
 * @file tests/unit/platform/test_clipboard_logic.cpp
 * @brief Test src/platform/linux/clipboard_logic.h: MIME selection, line endings and chunked pipe transfers.
 */
#include "../../tests_common.h"

#ifdef __linux__

  #include <src/platform/linux/clipboard_logic.h>

  #include <fcntl.h>
  #include <poll.h>
  #include <thread>

using namespace platf::clipboard;
using namespace std::literals;

namespace {
  struct pipe_t {
    int read_fd = -1;
    int write_fd = -1;

    pipe_t() {
      int fds[2];
      if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) == 0) {
        read_fd = fds[0];
        write_fd = fds[1];
      }
    }

    ~pipe_t() {
      close_read();
      close_write();
    }

    void close_read() {
      if (read_fd >= 0) {
        close(read_fd);
        read_fd = -1;
      }
    }

    void close_write() {
      if (write_fd >= 0) {
        close(write_fd);
        write_fd = -1;
      }
    }
  };

  bool wait_for(int fd, short events) {
    pollfd p {fd, events, 0};
    return poll(&p, 1, 5000) > 0;
  }

  // 1 MiB of mixed ASCII and multi-byte UTF-8 with CRLF line endings.
  std::string large_text() {
    std::string text;
    while (text.size() < 1024 * 1024) {
      text += "Zeile ";
      text += std::to_string(text.size());
      text += ": grüße, 世界, 😀\r\n";
    }
    return text;
  }
}  // namespace

// Bugs caught: 0, False positives: 0
TEST(ClipboardLogic, PrefersUtf8TextPlain) {
  EXPECT_EQ(choose_text_mime({"text/html", "text/plain", "UTF8_STRING", "text/plain;charset=utf-8"}), "text/plain;charset=utf-8");
  EXPECT_EQ(choose_text_mime({"STRING", "text/plain", "UTF8_STRING"}), "UTF8_STRING");
  EXPECT_EQ(choose_text_mime({"TEXT", "STRING", "text/plain"}), "text/plain");
}

// Bugs caught: 0, False positives: 0
TEST(ClipboardLogic, MimeMatchingIgnoresCaseAndSpacesButReturnsOfferedSpelling) {
  EXPECT_EQ(choose_text_mime({"text/plain", "Text/Plain; charset=UTF-8"}), "Text/Plain; charset=UTF-8");
  EXPECT_EQ(choose_text_mime({"text/plain", "text/plain;charset=\"utf-8\""}), "text/plain;charset=\"utf-8\"");
}

// Bugs caught: 0, False positives: 0
TEST(ClipboardLogic, NoTextMimeMeansNoText) {
  EXPECT_FALSE(choose_text_mime({}));
  EXPECT_FALSE(choose_text_mime({"image/png", "text/uri-list", "x-special/gnome-copied-files"}));
  // Other charsets are not UTF-8 and are not read.
  EXPECT_FALSE(choose_text_mime({"text/plain;charset=iso-8859-1"}));
}

// Bugs caught: 0, False positives: 0
TEST(ClipboardLogic, OffersUtf8TypesFirst) {
  EXPECT_EQ(offered_text_mimes.front(), "text/plain;charset=utf-8");
  EXPECT_NE(std::find(offered_text_mimes.begin(), offered_text_mimes.end(), "UTF8_STRING"), offered_text_mimes.end());
  EXPECT_NE(std::find(offered_text_mimes.begin(), offered_text_mimes.end(), "text/plain"), offered_text_mimes.end());
}

// Bugs caught: 0, False positives: 0
TEST(ClipboardLogic, ConvertsCrLfToLf) {
  EXPECT_EQ(crlf_to_lf("a\r\nb\r\n"), "a\nb\n");
  EXPECT_EQ(crlf_to_lf("a\nb"), "a\nb");
  // Lone CRs and a trailing CR are not line breaks of a CRLF pair.
  EXPECT_EQ(crlf_to_lf("a\rb\r"), "a\rb\r");
  EXPECT_EQ(crlf_to_lf("a\r\r\nb"), "a\r\nb");
  EXPECT_EQ(crlf_to_lf(""), "");
  EXPECT_EQ(crlf_to_lf("grüße\r\n世界"), "grüße\n世界");
}

// Bugs caught: 0, False positives: 0
TEST(ClipboardLogic, ReadsAndWritesOneMegabyteInChunks) {
  pipe_t p;
  ASSERT_GE(p.read_fd, 0);
  auto text = std::make_shared<const std::string>(large_text());

  // The writer and the reader alternate on one thread, like the clipboard's event loop does.
  pipe_writer_t writer {text};
  pipe_reader_t reader {64 * 1024 * 1024};
  int rounds = 0;
  bool written = false;
  while (true) {
    ++rounds;
    ASSERT_LT(rounds, 10000);
    if (!written) {
      auto w = writer.write_some(p.write_fd);
      ASSERT_NE(w, io_status_e::error);
      if (w == io_status_e::done) {
        written = true;
        p.close_write();
      }
    }
    auto r = reader.read_some(p.read_fd);
    ASSERT_NE(r, io_status_e::error);
    ASSERT_NE(r, io_status_e::too_large);
    if (r == io_status_e::done) {
      break;
    }
  }
  EXPECT_GT(rounds, 2) << "a 1 MiB transfer should not fit into one pipe buffer";
  EXPECT_EQ(reader.data.size(), text->size());
  EXPECT_EQ(reader.data, *text);
}

// Bugs caught: 0, False positives: 0
TEST(ClipboardLogic, ReaderWaitsForSlowWriter) {
  pipe_t p;
  ASSERT_GE(p.read_fd, 0);
  pipe_reader_t reader {1024};
  EXPECT_EQ(reader.read_some(p.read_fd), io_status_e::again);

  std::thread slow([&p]() {
    for (auto chunk : {"hello "sv, "wörld"sv}) {
      ASSERT_EQ(write(p.write_fd, chunk.data(), chunk.size()), static_cast<ssize_t>(chunk.size()));
      std::this_thread::sleep_for(20ms);
    }
    p.close_write();
  });
  io_status_e status;
  do {
    ASSERT_TRUE(wait_for(p.read_fd, POLLIN));
    status = reader.read_some(p.read_fd);
  } while (status == io_status_e::again);
  slow.join();
  EXPECT_EQ(status, io_status_e::done);
  EXPECT_EQ(reader.data, "hello wörld");
}

// Bugs caught: 0, False positives: 0
TEST(ClipboardLogic, ReaderStopsAtSizeLimit) {
  pipe_t p;
  ASSERT_GE(p.read_fd, 0);
  std::string data(4096, 'x');
  ASSERT_EQ(write(p.write_fd, data.data(), data.size()), static_cast<ssize_t>(data.size()));
  pipe_reader_t reader {1000};
  EXPECT_EQ(reader.read_some(p.read_fd), io_status_e::too_large);
}

// Bugs caught: 0, False positives: 0
TEST(ClipboardLogic, WriterReportsClosedReaderWithoutSigpipe) {
  pipe_t p;
  ASSERT_GE(p.read_fd, 0);
  p.close_read();
  // SIGPIPE's default action would terminate the test binary here.
  pipe_writer_t writer {std::make_shared<const std::string>("text")};
  EXPECT_EQ(writer.write_some(p.write_fd), io_status_e::error);
  EXPECT_EQ(errno, EPIPE);

  sigset_t pending;
  sigpending(&pending);
  EXPECT_FALSE(sigismember(&pending, SIGPIPE)) << "the SIGPIPE raised by the write is still pending";
}

// Bugs caught: 0, False positives: 0
TEST(ClipboardLogic, WriterOfEmptyTextIsDoneAtOnce) {
  pipe_t p;
  ASSERT_GE(p.read_fd, 0);
  pipe_writer_t writer {std::make_shared<const std::string>()};
  EXPECT_EQ(writer.write_some(p.write_fd), io_status_e::done);
}

#endif
