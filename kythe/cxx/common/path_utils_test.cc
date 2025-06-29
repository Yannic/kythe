/*
 * Copyright 2018 The Kythe Authors. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "kythe/cxx/common/path_utils.h"

#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <string>
#include <system_error>
#include <vector>

#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "kythe/cxx/common/regex.h"

namespace kythe {
namespace {
using ::testing::ElementsAre;
using ::testing::FieldsAre;
using ::testing::Property;
using ::testing::VariantWith;

std::error_code Symlink(const std::string& target,
                        const std::string& linkpath) {
  if (::symlink(target.c_str(), linkpath.c_str()) < 0) {
    return std::error_code(errno, std::generic_category());
  }
  return std::error_code();
}

std::error_code MakeDirectory(const std::string& path) {
  if (::mkdir(path.c_str(), 0755) < 0) {
    return std::error_code(errno, std::generic_category());
  }
  return std::error_code();
}

std::error_code Remove(const std::string& path) {
  if (::remove(path.c_str()) < 0) {
    return std::error_code(errno, std::generic_category());
  }
  return std::error_code();
}

TEST(PathUtilsTest, CleanPath) {
  EXPECT_EQ("/a/c", CleanPath("/../../a/c"));
  EXPECT_EQ("", CleanPath(""));
  // CleanPath should match the semantics of Go's path.Clean (except for
  // "" => "", not "" => "."); the examples from the documentation at
  // http://golang.org/pkg/path/#example_Clean are checked here.
  EXPECT_EQ("a/c", CleanPath("a/c"));
  EXPECT_EQ("a/c", CleanPath("./a/c"));
  EXPECT_EQ("a/c", CleanPath("a//c"));
  EXPECT_EQ("a/c", CleanPath("a/c/."));
  EXPECT_EQ("a/c", CleanPath("a/c/b/.."));
  EXPECT_EQ("/a/c", CleanPath("/../a/c"));
  EXPECT_EQ("/a/c", CleanPath("/../a/b/../././/c"));
  EXPECT_EQ("/Users", CleanPath("/Users"));
  // "//Users" denotes a path with the root name "//Users"
  EXPECT_EQ("//Users", CleanPath("//Users"));
  EXPECT_EQ("/Users", CleanPath("///Users"));
  EXPECT_EQ("..", CleanPath("a/../../"));
}

TEST(PathUtilsTest, JoinPath) {
  EXPECT_EQ("a/c", JoinPath("a", "c"));
  EXPECT_EQ("a/c", JoinPath("a/", "c"));
  EXPECT_EQ("a/c", JoinPath("a", "/c"));
}

TEST(PathUtilsTest, RelativizePath) {
  absl::StatusOr<std::string> current_dir = GetCurrentDirectory();
  ASSERT_TRUE(current_dir.ok());

  std::string cwd_foo = JoinPath(*current_dir, "foo");

  EXPECT_EQ("foo", RelativizePath("foo", "."));
  EXPECT_EQ("foo", RelativizePath("foo", *current_dir));
  EXPECT_EQ("foo", RelativizePath("./foo", "."));
  EXPECT_EQ("foo", RelativizePath("./foo", *current_dir));
  EXPECT_EQ("bar", RelativizePath("foo/bar", "foo"));
  EXPECT_EQ("bar", RelativizePath("foo/bar", cwd_foo));
  EXPECT_EQ("foo", RelativizePath(cwd_foo, "."));
  EXPECT_EQ(cwd_foo, RelativizePath(cwd_foo, "bar"));

  // If all paths are absolute, then relativizing is unaffected by current_dir.
  EXPECT_EQ("bar", RelativizePath("/foo/bar", "/foo"));
  EXPECT_EQ("foo", RelativizePath("/foo", "/"));

  // Test that we only accept proper path prefixes as parent.
  EXPECT_EQ("/foooo/bar", RelativizePath("/foooo/bar", "/foo"));
}

TEST(PathUtilsTest, MakeCleanAbsolutePath) {
  std::string current_dir = GetCurrentDirectory().value();

  EXPECT_EQ(current_dir, MakeCleanAbsolutePath(".").value());

  EXPECT_EQ("/a/b/c", MakeCleanAbsolutePath("/a/b/c").value());
  EXPECT_EQ("/a/b/c", MakeCleanAbsolutePath("/a/b/c/.").value());
  EXPECT_EQ("/a/b", MakeCleanAbsolutePath("/a/b/c/./..").value());
  EXPECT_EQ("/a/b", MakeCleanAbsolutePath("/a/b/c/../.").value());
  EXPECT_EQ("/a/b", MakeCleanAbsolutePath("/a/b/c/..").value());
  EXPECT_EQ("/", MakeCleanAbsolutePath("/a/../c/..").value());
  EXPECT_EQ("/", MakeCleanAbsolutePath("/a/b/c/../../..").value());
}

TEST(PathUtilsTest, RealPath) {
  // Since RealPath accesses the filesystem, we can't make very many
  // guarantees about its behavior, but we would like it to return an
  // error if a path doesn't exist.
  absl::StatusOr<std::string> result = RealPath("/this/path/should/not/exist");
  EXPECT_EQ(absl::StatusCode::kNotFound, result.status().code());

  // Some systems symlink their temporary directory, so use RealPath()
  // here to deal with that.
  const std::string temp_dir = RealPath(testing::TempDir()).value();

  // If so, check that RealPath resolves a known link.
  const std::string link_path = JoinPath(temp_dir, "PathUtilsTestLink");
  ASSERT_EQ(std::error_code(), Symlink(temp_dir, link_path));
  result = RealPath(link_path);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(temp_dir, *result);
  EXPECT_EQ(std::error_code(), Remove(link_path));
}

TEST(PathUtilsTest, CanonicalizerPolicyParseTest) {
  std::string error;
  PathCanonicalizer::Policy policy;
  ASSERT_TRUE(AbslParseFlag("clean-only", &policy, &error));
  EXPECT_EQ(PathCanonicalizer::Policy::kCleanOnly, policy);
  ASSERT_TRUE(AbslParseFlag("prefer-relative", &policy, &error));
  EXPECT_EQ(PathCanonicalizer::Policy::kPreferRelative, policy);
  ASSERT_TRUE(AbslParseFlag("prefer-real", &policy, &error));
  EXPECT_EQ(PathCanonicalizer::Policy::kPreferReal, policy);
  ASSERT_FALSE(AbslParseFlag("some-random-junk", &policy, &error));
  EXPECT_NE("", error);
}

TEST(PathUtilsTest, CanonicalizerPathEntryParseTest) {
  std::string error;
  std::vector<PathCanonicalizer::PathEntry> entries;
  ASSERT_TRUE(
      AbslParseFlag("a@clean-only b@prefer-relative", &entries, &error));
  EXPECT_THAT(
      entries,
      ElementsAre(FieldsAre(VariantWith<Regex>(Property(&Regex::pattern, "a")),
                            PathCanonicalizer::Policy::kCleanOnly),
                  FieldsAre(VariantWith<Regex>(Property(&Regex::pattern, "b")),
                            PathCanonicalizer::Policy::kPreferRelative)));
  EXPECT_EQ(AbslUnparseFlag(entries), "a@clean-only b@prefer-relative");
}

TEST(PathUtilsTest, CanonicalizerPathEntryParseFailuresTest) {
  std::string error;
  std::vector<PathCanonicalizer::PathEntry> entries;
  ASSERT_FALSE(AbslParseFlag("clean-only b@prefer-relative", &entries, &error));
  EXPECT_THAT(error, "missing @ delimiter between path and policy");
}

class CanonicalizerTest : public ::testing::Test {
 public:
  void SetUp() override {
    const testing::TestInfo* test_info =
        testing::UnitTest::GetInstance()->current_test_info();
    std::string root = JoinPath(JoinPath(RealPath(testing::TempDir()).value(),
                                         test_info->test_suite_name()),
                                test_info->name());
    ASSERT_EQ(std::error_code(), MakeDirectory(root));
    filesystem_.push_back(root);
  }

  void TearDown() override {
    std::sort(filesystem_.rbegin(), filesystem_.rend());
    for (const std::string& path : filesystem_) {
      EXPECT_EQ(std::error_code(), Remove(path));
    }
  }

  void AddDirectory(const std::string& name) {
    std::string path = JoinPath(root(), name);
    ASSERT_EQ(std::error_code(), MakeDirectory(path));
    filesystem_.push_back(path);
  }

  void AddSymlink(const std::string& target, const std::string& name) {
    std::string path = JoinPath(root(), name);
    ASSERT_EQ(std::error_code(), Symlink(target, path));
    filesystem_.push_back(path);
  }

  const std::string& root() { return filesystem_.front(); }

  static void SetUpTestSuite() {
    CHECK_EQ(std::error_code(),
             MakeDirectory(JoinPath(testing::TempDir(), "CanonicalizerTest")));
  }

  static void TearDownTestSuite() {
    CHECK_EQ(std::error_code(),
             Remove(JoinPath(testing::TempDir(), "CanonicalizerTest")));
  }

 private:
  std::vector<std::string> filesystem_;
};

TEST_F(CanonicalizerTest, CanonicalizerCleanPathOnly) {
  AddDirectory("concrete");
  AddDirectory("concrete/subdir");
  AddDirectory("concrete/file");
  AddSymlink("concrete", "link");

  PathCanonicalizer canonicalizer =
      PathCanonicalizer::Create(root(), PathCanonicalizer::Policy::kCleanOnly)
          .value();
  EXPECT_EQ("link/file",
            canonicalizer.Relativize(JoinPath(root(), "link/subdir/../file"))
                .value_or(""));
  EXPECT_EQ("link/file",
            canonicalizer.Relativize(JoinPath(root(), "./link/subdir/../file"))
                .value_or(""));
}

TEST_F(CanonicalizerTest, CanonicalizerPreferRelative) {
  AddDirectory("base");
  AddDirectory("elsewhere");
  AddDirectory("elsewhere/subdir");
  AddDirectory("elsewhere/file");
  AddSymlink("../elsewhere", "base/link");

  const std::string base = JoinPath(root(), "base");
  PathCanonicalizer canonicalizer =
      PathCanonicalizer::Create(base,
                                PathCanonicalizer::Policy::kPreferRelative)
          .value();
  // link/file points somewhere outside of "base", so prefer the relative path,
  // even if it's an unresolved symlink.
  EXPECT_EQ("link/file",
            canonicalizer.Relativize(JoinPath(base, "link/subdir/../file"))
                .value_or(""));
}

TEST_F(CanonicalizerTest, CanonicalizerPreferReal) {
  AddDirectory("base");
  AddDirectory("elsewhere");
  AddDirectory("elsewhere/subdir");
  AddDirectory("elsewhere/file");
  AddSymlink("../elsewhere", "base/link");

  const std::string base = JoinPath(root(), "base");
  PathCanonicalizer canonicalizer =
      PathCanonicalizer::Create(base, PathCanonicalizer::Policy::kPreferReal)
          .value();
  // Use the resolved path, even if it points outside of the base.
  EXPECT_EQ(JoinPath(root(), "elsewhere/file"),
            canonicalizer.Relativize(JoinPath(base, "link/subdir/../file"))
                .value_or(""));
  // Unless the link is bad, then use the cleaned path.
  EXPECT_EQ("bad/file",
            canonicalizer.Relativize(JoinPath(base, "bad/subdir/../file"))
                .value_or(""));
}

TEST_F(CanonicalizerTest, CanonicalizerRealOverrideClean) {
  AddDirectory("concrete");
  AddDirectory("concrete/subdir");
  AddDirectory("concrete/file");
  AddSymlink("concrete", "link");

  AddDirectory("base");
  AddDirectory("elsewhere");
  AddDirectory("elsewhere/subdir");
  AddDirectory("elsewhere/file");
  AddSymlink("../elsewhere", "base/link");

  const std::string base = JoinPath(root(), "base");
  PathCanonicalizer canonicalizer =
      PathCanonicalizer::Create(
          root(), PathCanonicalizer::Policy::kCleanOnly,
          {{.path = ".*/base/link",
            .policy = PathCanonicalizer::Policy::kPreferReal}})
          .value();

  // Use the default clean-only policy for links/paths which don't match.
  EXPECT_EQ("link/file",
            canonicalizer.Relativize(JoinPath(root(), "link/subdir/../file"))
                .value_or(""));
  EXPECT_EQ("link/file",
            canonicalizer.Relativize(JoinPath(root(), "./link/subdir/../file"))
                .value_or(""));
  // And the prefer-real policy for those which do.
  EXPECT_EQ("elsewhere/file",
            canonicalizer.Relativize(JoinPath(base, "link/subdir/../file"))
                .value_or(""));
  EXPECT_EQ("base/bad/file",
            canonicalizer.Relativize(JoinPath(base, "bad/subdir/../file"))
                .value_or(""));
}

TEST_F(CanonicalizerTest,
       PathCleanerRelativizeNonAbsoluteNoPrefix_Relativizes) {
  // Original path "some/other/path" is relative.
  // Cleaner root is `root()` (e.g. /tmp/test_fixture/test_name)
  // Cleaned absolute path of input will be `root()` + "some/other/path".
  // This cleaned absolute path *does* start with `root()`.
  // Thus, it should proceed to relativize.
  PathCleaner cleaner = PathCleaner::Create(root()).value();
  EXPECT_EQ("some/other/path",  // Relativized form of root() +
                                // "some/other/path" against root()
            cleaner.Relativize("some/other/path").value_or(""));
}

TEST_F(CanonicalizerTest,
       PathCleanerRelativizeNonAbsoluteWithAbsolutePrefixInCleanerRoot) {
  // Input path is absolute and shares a prefix with root().
  // This should be relativized.
  PathCleaner cleaner = PathCleaner::Create(root()).value();
  std::string path_inside_root =
      JoinPath(root(), "some/path");  // Creates an absolute path
  EXPECT_EQ("some/path", cleaner.Relativize(path_inside_root).value_or(""));
}

TEST_F(CanonicalizerTest, PathCleanerRelativizeAbsoluteDifferentRootNoPrefix) {
  PathCleaner cleaner = PathCleaner::Create(root()).value();
  // Input path is absolute "/other/root/file". Cleaner root is `root()`.
  // They do not share a prefix.
  // We should see the original path returned.
  EXPECT_EQ("/other/root/file",
            cleaner.Relativize("/other/root/file").value_or(""));
}

TEST_F(CanonicalizerTest, PathCleanerDoesntCleanRelativePaths) {
  PathCleaner cleaner = PathCleaner::Create(root()).value();
  EXPECT_EQ("../../foo.cc", cleaner.Relativize("../../foo.cc").value_or(""));
}

}  // namespace
}  // namespace kythe
