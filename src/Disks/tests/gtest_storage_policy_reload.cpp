#include <gtest/gtest.h>

#include <Common/ErrorCodes.h>
#include <Common/Exception.h>
#include <Common/tests/gtest_global_context.h>
#include <Disks/DiskLocal.h>
#include <Disks/DiskSelector.h>
#include <Disks/IVolume.h>
#include <Disks/StoragePolicy.h>

#include <Poco/AutoPtr.h>
#include <Poco/Util/XMLConfiguration.h>

#include <atomic>
#include <filesystem>
#include <sstream>
#include <unistd.h>

namespace DB::ErrorCodes
{
    extern const int NO_ELEMENTS_IN_CONFIG;
}

namespace
{

namespace fs = std::filesystem;

Poco::AutoPtr<Poco::Util::XMLConfiguration> parseXmlConfig(const std::string & xml)
{
    std::istringstream in(xml);
    return Poco::AutoPtr<Poco::Util::XMLConfiguration>(new Poco::Util::XMLConfiguration(in));
}

class StoragePolicyReloadTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        static std::atomic<size_t> seq{0};
        root = fs::temp_directory_path()
            / ("clickhouse_gtest_storage_policy_reload_" + std::to_string(::getpid()) + "_" + std::to_string(++seq));
        fs::remove_all(root);
        fs::create_directories(root / "default");
        fs::create_directories(root / "hot");

        auto seed_cfg = parseXmlConfig("<clickhouse><storage_configuration><disks><default/></disks></storage_configuration></clickhouse>");
        auto mutable_disks = std::make_shared<DB::DiskSelector>();
        mutable_disks->initialize(
            *seed_cfg,
            "storage_configuration.disks",
            getContext().context,
            [](const Poco::Util::AbstractConfiguration &, const DB::String &, const DB::String &) { return false; });
        mutable_disks->addToDiskMap("default", std::make_shared<DB::DiskLocal>("default", (root / "default").string() + "/"));
        mutable_disks->addToDiskMap("hot", std::make_shared<DB::DiskLocal>("hot", (root / "hot").string() + "/"));
        disks = mutable_disks;
    }

    void TearDown() override
    {
        if (disks)
        {
            for (const auto & [disk_name, disk] : disks->getDisksMap())
            {
                (void)disk_name;
                disk->shutdown();
            }
            disks.reset();
        }
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    fs::path root;
    DB::DiskSelectorPtr disks;
};

}

TEST_F(StoragePolicyReloadTest, OmittedDefaultPolicyReloadDoesNotIncrementNoElementsInConfigError)
{
    const auto cfg_without_default = parseXmlConfig(R"xml(
        <clickhouse>
            <storage_configuration>
                <policies>
                    <cas_hot_cold>
                        <volumes>
                            <hot>
                                <disk>hot</disk>
                            </hot>
                        </volumes>
                    </cas_hot_cold>
                </policies>
            </storage_configuration>
        </clickhouse>
    )xml");

    auto selector = std::make_shared<DB::StoragePolicySelector>(*cfg_without_default, "storage_configuration.policies", disks);
    ASSERT_TRUE(selector->get("default")->isDefaultPolicy());

    const size_t errors_before = DB::ErrorCodes::values[DB::ErrorCodes::NO_ELEMENTS_IN_CONFIG].get().local.count;

    for (int i = 0; i < 3; ++i)
    {
        DB::Strings new_disks;
        selector = selector->updateFromConfig(*cfg_without_default, "storage_configuration.policies", disks, new_disks);
        EXPECT_TRUE(selector->get("default")->isDefaultPolicy());
        EXPECT_NE(selector->tryGet("cas_hot_cold"), nullptr);
    }

    const auto cfg_empty_default_tag = parseXmlConfig(R"xml(
        <clickhouse>
            <storage_configuration>
                <policies>
                    <default/>
                    <cas_hot_cold>
                        <volumes>
                            <hot>
                                <disk>hot</disk>
                            </hot>
                        </volumes>
                    </cas_hot_cold>
                </policies>
            </storage_configuration>
        </clickhouse>
    )xml");
    {
        DB::Strings new_disks;
        selector = selector->updateFromConfig(*cfg_empty_default_tag, "storage_configuration.policies", disks, new_disks);
        EXPECT_TRUE(selector->get("default")->isDefaultPolicy());
    }

    const size_t errors_after = DB::ErrorCodes::values[DB::ErrorCodes::NO_ELEMENTS_IN_CONFIG].get().local.count;
    EXPECT_EQ(errors_after, errors_before);
}

TEST_F(StoragePolicyReloadTest, PartiallyConfiguredInvalidDefaultPolicyStillThrowsNoElementsInConfig)
{
    const auto empty_cfg = parseXmlConfig("<clickhouse/>");
    auto old_default = std::make_shared<DB::StoragePolicy>("default", *empty_cfg, "storage_configuration.policies.default", disks);
    ASSERT_TRUE(old_default->isDefaultPolicy());

    for (const char * invalid_xml : {
             R"xml(
                 <clickhouse>
                     <storage_configuration>
                         <policies>
                             <default>
                                 <move_factor>0.2</move_factor>
                             </default>
                         </policies>
                     </storage_configuration>
                 </clickhouse>
             )xml",
             R"xml(
                 <clickhouse>
                     <storage_configuration>
                         <policies>
                             <default>
                                 <volumes/>
                             </default>
                         </policies>
                     </storage_configuration>
                 </clickhouse>
             )xml",
         })
    {
        const auto invalid_cfg = parseXmlConfig(invalid_xml);
        try
        {
            DB::StoragePolicy reloaded(old_default, *invalid_cfg, "storage_configuration.policies.default", disks);
            FAIL() << "Expected NO_ELEMENTS_IN_CONFIG for invalid default policy config";
        }
        catch (const DB::Exception & e)
        {
            EXPECT_EQ(e.code(), DB::ErrorCodes::NO_ELEMENTS_IN_CONFIG);
        }
    }

    const auto valid_explicit_default_cfg = parseXmlConfig(R"xml(
        <clickhouse>
            <storage_configuration>
                <policies>
                    <default>
                        <volumes>
                            <default>
                                <disk>default</disk>
                                <max_data_part_size_bytes>16777216</max_data_part_size_bytes>
                            </default>
                        </volumes>
                    </default>
                </policies>
            </storage_configuration>
        </clickhouse>
    )xml");

    const size_t errors_before = DB::ErrorCodes::values[DB::ErrorCodes::NO_ELEMENTS_IN_CONFIG].get().local.count;
    DB::StoragePolicy updated_default(old_default, *valid_explicit_default_cfg, "storage_configuration.policies.default", disks);
    const size_t errors_after = DB::ErrorCodes::values[DB::ErrorCodes::NO_ELEMENTS_IN_CONFIG].get().local.count;

    EXPECT_EQ(errors_after, errors_before);
    EXPECT_EQ(updated_default.getVolumeByName("default")->max_data_part_size, 16777216ULL);
}
