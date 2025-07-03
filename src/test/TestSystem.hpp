// File: src/test/TestSystem.hpp
#pragma once

#include <string>
#include <vector>
#include <functional>

namespace Test {

    // Test result structure
    struct TestResult {
        std::string testName;
        bool passed;
        std::string message;
        double executionTimeMs;

        TestResult(const std::string& name, bool success, const std::string& msg = "", double timeMs = 0.0)
            : testName(name), passed(success), message(msg), executionTimeMs(timeMs) {}
    };

    // Test suite results
    struct TestSuiteResult {
        std::string suiteName;
        std::vector<TestResult> tests;
        int totalTests = 0;
        int passedTests = 0;
        int failedTests = 0;
        double totalTimeMs = 0.0;

        void AddResult(const TestResult& result) {
            tests.push_back(result);
            totalTests++;
            totalTimeMs += result.executionTimeMs;

            if (result.passed) {
                passedTests++;
            } else {
                failedTests++;
            }
        }

        bool AllPassed() const {
            return failedTests == 0;
        }
    };

    // Main test system class
    class TestSystem {
    public:
        // Initialize the test system
        static void Initialize();

        // Run all registered tests
        static void RunAllTests();

        // Run a specific test suite
        static TestSuiteResult RunTestSuite(const std::string& suiteName);

        // Quick test runner - call this from PlatformMain.cpp
        static bool QuickTest();

        // Register test suites
        static void RegisterTestSuite(const std::string& name, std::function<TestSuiteResult()> suite);

        // Individual test functions (call these from your test suites)
        static TestResult TestBlockModelLoading();
        static TestResult TestBlockModelParsing();
        static TestResult TestTextureResolution();
        static TestResult TestBiomeTinting();
        static TestResult TestModelRegistration();
        static TestResult TestDefaultModels();
        static TestResult TestJSONErrorHandling();

        // Utility functions for tests
        static bool CreateTestModelFile(const std::string& filename, const std::string& jsonContent);
        static void CleanupTestFiles();

    private:
        static void PrintTestResults(const TestSuiteResult& results);
        static double GetCurrentTimeMs();

        // Test data
        static std::vector<std::pair<std::string, std::function<TestSuiteResult()>>> s_testSuites;
    };

    // Helper macros for easier test writing
    #define TEST_ASSERT(condition, message) \
        do { \
            if (!(condition)) { \
                return TestResult(__func__, false, std::string("ASSERTION FAILED: ") + (message)); \
            } \
        } while(0)

    #define TEST_ASSERT_EQ(expected, actual, message) \
        do { \
            if ((expected) != (actual)) { \
                return TestResult(__func__, false, std::string("ASSERTION FAILED: ") + (message) + \
                    " (expected: " + std::to_string(expected) + ", actual: " + std::to_string(actual) + ")"); \
            } \
        } while(0)

    #define TEST_ASSERT_STREQ(expected, actual, message) \
        do { \
            if ((expected) != (actual)) { \
                return TestResult(__func__, false, std::string("ASSERTION FAILED: ") + (message) + \
                    " (expected: \"" + (expected) + "\", actual: \"" + (actual) + "\")"); \
            } \
        } while(0)

    #define TEST_SUCCESS(message) \
        return TestResult(__func__, true, message)

} // namespace Test