// File: src/test/TestSystem.cpp
#include "TestSystem.hpp"
#include "../core/Log.hpp"
#include "../game/BlockModel.hpp"
#include "../game/BlockRegistry.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>

namespace Test {

    // Static member definitions
    std::vector<std::pair<std::string, std::function<TestSuiteResult()>>> TestSystem::s_testSuites;

    void TestSystem::Initialize() {
        Log::Info("=== INITIALIZING TEST SYSTEM ===");

        // Register our test suites
        RegisterTestSuite("BlockModels", []() {
            TestSuiteResult suite;
            suite.suiteName = "Block Model System";

            auto startTime = GetCurrentTimeMs();

            // Run individual tests
            suite.AddResult(TestBlockModelLoading());
            suite.AddResult(TestBlockModelParsing());
            suite.AddResult(TestTextureResolution());
            suite.AddResult(TestBiomeTinting());
            suite.AddResult(TestModelRegistration());
            suite.AddResult(TestDefaultModels());
            suite.AddResult(TestJSONErrorHandling());

            suite.totalTimeMs = GetCurrentTimeMs() - startTime;

            return suite;
        });

        Log::Info("Test system initialized with %zu test suites", s_testSuites.size());
    }

    bool TestSystem::QuickTest() {
        Log::Info("=== RUNNING QUICK TESTS ===");

        Initialize();

        bool allPassed = true;

        // Run all test suites
        for (const auto& [name, suite] : s_testSuites) {
            auto results = suite();
            PrintTestResults(results);

            if (!results.AllPassed()) {
                allPassed = false;
            }
        }

        // Cleanup
        CleanupTestFiles();

        Log::Info("=== QUICK TEST COMPLETE: %s ===", allPassed ? "ALL PASSED" : "SOME FAILED");
        return allPassed;
    }

    void TestSystem::RunAllTests() {
        Initialize();

        Log::Info("=== RUNNING ALL TESTS ===");

        int totalSuites = 0;
        int passedSuites = 0;
        double totalTime = 0.0;

        for (const auto& [name, suite] : s_testSuites) {
            totalSuites++;
            auto results = suite();
            PrintTestResults(results);
            totalTime += results.totalTimeMs;

            if (results.AllPassed()) {
                passedSuites++;
            }
        }

        Log::Info("=== ALL TESTS COMPLETE ===");
        Log::Info("Test Suites: %d/%d passed", passedSuites, totalSuites);
        Log::Info("Total execution time: %.2f ms", totalTime);

        CleanupTestFiles();
    }

    TestResult TestSystem::TestBlockModelLoading() {
        auto startTime = GetCurrentTimeMs();

        // Test basic model loading
        bool success = Game::BlockModelRegistry::LoadModels("assets/models/block");

        if (!success) {
            return TestResult(__func__, false, "Failed to load block models from assets/models/block",
                            GetCurrentTimeMs() - startTime);
        }

        size_t modelCount = Game::BlockModelRegistry::GetModelCount();
        if (modelCount == 0) {
            return TestResult(__func__, false, "No models were loaded",
                            GetCurrentTimeMs() - startTime);
        }

        Log::Info("Loaded %zu block models", modelCount);

        return TestResult(__func__, true, "Successfully loaded " + std::to_string(modelCount) + " models",
                        GetCurrentTimeMs() - startTime);
    }

    TestResult TestSystem::TestBlockModelParsing() {
        auto startTime = GetCurrentTimeMs();

        // Create a test grass block model
        std::string testJson = R"({
            "parent": "block/block",
            "textures": {
                "particle": "block/dirt",
                "bottom": "block/dirt",
                "top": "block/grass_block_top",
                "side": "block/grass_block_side",
                "overlay": "block/grass_block_side_overlay"
            },
            "elements": [
                {
                    "from": [ 0, 0, 0 ],
                    "to": [ 16, 16, 16 ],
                    "faces": {
                        "down":  { "uv": [ 0, 0, 16, 16 ], "texture": "#bottom", "cullface": "down" },
                        "up":    { "uv": [ 0, 0, 16, 16 ], "texture": "#top", "cullface": "up", "tintindex": 0 },
                        "north": { "uv": [ 0, 0, 16, 16 ], "texture": "#side", "cullface": "north" },
                        "south": { "uv": [ 0, 0, 16, 16 ], "texture": "#side", "cullface": "south" },
                        "west":  { "uv": [ 0, 0, 16, 16 ], "texture": "#side", "cullface": "west" },
                        "east":  { "uv": [ 0, 0, 16, 16 ], "texture": "#side", "cullface": "east" }
                    }
                }
            ]
        })";

        if (!CreateTestModelFile("test_grass.json", testJson)) {
            return TestResult(__func__, false, "Failed to create test model file",
                            GetCurrentTimeMs() - startTime);
        }

        // Reload models to include our test file
        Game::BlockModelRegistry::LoadModels("assets/models/block");

        // Test if our model was loaded correctly
        if (!Game::BlockModelRegistry::HasModel("test_grass")) {
            return TestResult(__func__, false, "Test grass model was not loaded",
                            GetCurrentTimeMs() - startTime);
        }

        const auto& model = Game::BlockModelRegistry::GetModel("test_grass");

        // Verify structure
        TEST_ASSERT_EQ(5, model.textures.size(), "Expected 5 texture definitions");
        TEST_ASSERT_EQ(1, model.elements.size(), "Expected 1 element");

        const auto& element = model.elements[0];
        TEST_ASSERT_EQ(6, element.faces.size(), "Expected 6 faces");

        // Check specific properties
        TEST_ASSERT(element.HasFace(Game::FaceDir::Up), "Should have up face");
        const auto& upFace = element.GetFace(Game::FaceDir::Up);
        TEST_ASSERT_EQ(0, upFace.tintIndex, "Up face should have tint index 0");
        TEST_ASSERT_STREQ("#top", upFace.textureRef, "Up face should reference #top texture");

        return TestResult(__func__, true, "Successfully parsed test grass model",
                        GetCurrentTimeMs() - startTime);
    }

    TestResult TestSystem::TestTextureResolution() {
        auto startTime = GetCurrentTimeMs();

        // Use the test model from previous test
        if (!Game::BlockModelRegistry::HasModel("test_grass")) {
            return TestResult(__func__, false, "Test grass model not available",
                            GetCurrentTimeMs() - startTime);
        }

        const auto& model = Game::BlockModelRegistry::GetModel("test_grass");

        // Test texture resolution
        std::string sideTexture = model.ResolveTexture("#side");
        std::string topTexture = model.ResolveTexture("#top");
        std::string bottomTexture = model.ResolveTexture("#bottom");

        TEST_ASSERT_STREQ("block/grass_block_side", sideTexture, "Side texture resolution");
        TEST_ASSERT_STREQ("block/grass_block_top", topTexture, "Top texture resolution");
        TEST_ASSERT_STREQ("block/dirt", bottomTexture, "Bottom texture resolution");

        // Test non-reference texture
        std::string directTexture = model.ResolveTexture("block/stone");
        TEST_ASSERT_STREQ("block/stone", directTexture, "Direct texture should pass through");

        // Test missing reference
        std::string missingTexture = model.ResolveTexture("#nonexistent");
        TEST_ASSERT_STREQ("missingno", missingTexture, "Missing texture should return fallback");

        return TestResult(__func__, true, "Texture resolution working correctly",
                        GetCurrentTimeMs() - startTime);
    }

    TestResult TestSystem::TestBiomeTinting() {
        auto startTime = GetCurrentTimeMs();

        if (!Game::BlockModelRegistry::HasModel("test_grass")) {
            return TestResult(__func__, false, "Test grass model not available",
                            GetCurrentTimeMs() - startTime);
        }

        const auto& model = Game::BlockModelRegistry::GetModel("test_grass");

        // Test biome tinting detection
        bool usesTinting = model.UsesBiomeTinting();
        TEST_ASSERT(usesTinting, "Grass model should use biome tinting");

        // Check specific faces for tinting
        const auto& element = model.elements[0];
        const auto& upFace = element.GetFace(Game::FaceDir::Up);
        TEST_ASSERT_EQ(0, upFace.tintIndex, "Up face should have tint index 0");

        const auto& downFace = element.GetFace(Game::FaceDir::Down);
        TEST_ASSERT_EQ(-1, downFace.tintIndex, "Down face should not be tinted");

        return TestResult(__func__, true, "Biome tinting detection working correctly",
                        GetCurrentTimeMs() - startTime);
    }

    TestResult TestSystem::TestModelRegistration() {
        auto startTime = GetCurrentTimeMs();

        size_t initialCount = Game::BlockModelRegistry::GetModelCount();

        // Get list of loaded models
        auto modelNames = Game::BlockModelRegistry::GetLoadedModelNames();

        TEST_ASSERT(modelNames.size() > 0, "Should have loaded model names");
        TEST_ASSERT_EQ(initialCount, modelNames.size(), "Model count should match name list size");

        // Test that we can retrieve each model
        for (const auto& name : modelNames) {
            TEST_ASSERT(Game::BlockModelRegistry::HasModel(name), "Should have model: " + name);

            const auto& model = Game::BlockModelRegistry::GetModel(name);
            // Basic validation - should have at least one element
            TEST_ASSERT(model.elements.size() > 0, "Model " + name + " should have elements");
        }

        return TestResult(__func__, true, "Model registration working correctly",
                        GetCurrentTimeMs() - startTime);
    }

    TestResult TestSystem::TestDefaultModels() {
        auto startTime = GetCurrentTimeMs();

        // Test getting a non-existent model (should return default)
        const auto& defaultModel = Game::BlockModelRegistry::GetModel("nonexistent_model_12345");

        TEST_ASSERT(defaultModel.elements.size() > 0, "Default model should have elements");
        TEST_ASSERT_EQ(1, defaultModel.elements.size(), "Default model should have exactly 1 element");

        const auto& element = defaultModel.elements[0];
        TEST_ASSERT_EQ(6, element.faces.size(), "Default model should have 6 faces");

        // Check default element bounds (should be full cube)
        TEST_ASSERT_EQ(0.0f, element.from.x, "Default element should start at 0,0,0");
        TEST_ASSERT_EQ(0.0f, element.from.y, "Default element should start at 0,0,0");
        TEST_ASSERT_EQ(0.0f, element.from.z, "Default element should start at 0,0,0");
        TEST_ASSERT_EQ(16.0f, element.to.x, "Default element should end at 16,16,16");
        TEST_ASSERT_EQ(16.0f, element.to.y, "Default element should end at 16,16,16");
        TEST_ASSERT_EQ(16.0f, element.to.z, "Default element should end at 16,16,16");

        return TestResult(__func__, true, "Default model system working correctly",
                        GetCurrentTimeMs() - startTime);
    }

    TestResult TestSystem::TestJSONErrorHandling() {
        auto startTime = GetCurrentTimeMs();

        // Test with invalid JSON
        std::string invalidJson = "{ invalid json syntax }";

        if (!CreateTestModelFile("test_invalid.json", invalidJson)) {
            return TestResult(__func__, false, "Failed to create invalid test file",
                            GetCurrentTimeMs() - startTime);
        }

        // This should not crash and should handle the error gracefully
        size_t beforeCount = Game::BlockModelRegistry::GetModelCount();
        Game::BlockModelRegistry::LoadModels("assets/models/block");
        size_t afterCount = Game::BlockModelRegistry::GetModelCount();

        // The invalid model should not be loaded, but the system should continue
        // (We can't easily test the exact count since other valid models will load)

        // Test getting the invalid model (should return default)
        const auto& invalidModel = Game::BlockModelRegistry::GetModel("test_invalid");
        TEST_ASSERT(invalidModel.elements.size() > 0, "Invalid model should fallback to default");

        return TestResult(__func__, true, "JSON error handling working correctly",
                        GetCurrentTimeMs() - startTime);
    }

    bool TestSystem::CreateTestModelFile(const std::string& filename, const std::string& jsonContent) {
        std::filesystem::create_directories("assets/models/block");

        std::string fullPath = "assets/models/block/" + filename;
        std::ofstream file(fullPath);

        if (!file.is_open()) {
            Log::Error("Failed to create test file: %s", fullPath.c_str());
            return false;
        }

        file << jsonContent;
        file.close();

        Log::Debug("Created test model file: %s", fullPath.c_str());
        return true;
    }

    void TestSystem::CleanupTestFiles() {
        // Remove test files we created
        std::vector<std::string> testFiles = {
            "assets/models/block/test_grass.json",
            "assets/models/block/test_invalid.json"
        };

        for (const auto& file : testFiles) {
            if (std::filesystem::exists(file)) {
                std::filesystem::remove(file);
                Log::Debug("Cleaned up test file: %s", file.c_str());
            }
        }
    }

    void TestSystem::RegisterTestSuite(const std::string& name, std::function<TestSuiteResult()> suite) {
        s_testSuites.emplace_back(name, suite);
    }

    void TestSystem::PrintTestResults(const TestSuiteResult& results) {
        Log::Info("=== TEST SUITE: %s ===", results.suiteName.c_str());

        for (const auto& test : results.tests) {
            const char* status = test.passed ? "PASS" : "FAIL";
            const char* color = test.passed ? "\033[32m" : "\033[31m";
            const char* reset = "\033[0m";

            if (test.passed) {
                Log::Info("  %s[%s]%s %s (%.2f ms) - %s",
                         color, status, reset, test.testName.c_str(),
                         test.executionTimeMs, test.message.c_str());
            } else {
                Log::Error("  %s[%s]%s %s (%.2f ms) - %s",
                          color, status, reset, test.testName.c_str(),
                          test.executionTimeMs, test.message.c_str());
            }
        }

        const char* suiteColor = results.AllPassed() ? "\033[32m" : "\033[31m";
        const char* reset = "\033[0m";

        Log::Info("=== SUITE SUMMARY: %s%d/%d PASSED%s (%.2f ms total) ===",
                 suiteColor, results.passedTests, results.totalTests, reset, results.totalTimeMs);
    }

    double TestSystem::GetCurrentTimeMs() {
        auto now = std::chrono::high_resolution_clock::now();
        auto duration = now.time_since_epoch();
        return std::chrono::duration<double, std::milli>(duration).count();
    }

} // namespace Test