/*
 * Unit tests for orchagent "System is ready" signalling:
 *   - BufferOrch::areAllPortsReady()
 *   - orchdaemon m_swssReady gate written to STATE_DB FEATURE|swss:up_status
 *
 * areAllPortsReady() scans m_ready_list (pre-populated from CONFIG_DB at startup
 * by initBufferReadyList) and returns true only when every entry is true.
 * The orchdaemon writes up_status=true once PORT_CONFIG_DONE AND
 * areAllPortsReady() are both satisfied.
 */

#define private public
#define protected public
#include "bufferorch.h"
#undef private
#undef protected

#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_table.h"
#include "mock_response_publisher.h"

extern string gMySwitchType;
extern std::unique_ptr<MockResponsePublisher> gMockResponsePublisher;

namespace swss_readiness_test
{
    using namespace std;
    using namespace swss;

    // -----------------------------------------------------------------------
    // Shared test fixture — mirrors bufferorch_ut.cpp setup
    // -----------------------------------------------------------------------
    class SwssReadinessTest : public ::testing::Test
    {
    public:
        shared_ptr<DBConnector> m_app_db;
        shared_ptr<DBConnector> m_config_db;
        shared_ptr<DBConnector> m_state_db;
        shared_ptr<DBConnector> m_app_state_db;
        shared_ptr<DBConnector> m_counters_db;
        shared_ptr<DBConnector> m_chassis_app_db;

        void SetUp() override
        {
            map<string, string> profile = {
                { "SAI_VS_SWITCH_TYPE", "SAI_VS_SWITCH_TYPE_BCM56850" },
                { "KV_DEVICE_MAC_ADDRESS", "20:03:04:05:06:00" }
            };
            ut_helper::initSaiApi(profile);

            m_app_db     = make_shared<DBConnector>("APPL_DB",   0);
            m_config_db  = make_shared<DBConnector>("CONFIG_DB", 0);
            m_state_db   = make_shared<DBConnector>("STATE_DB",  0);
            m_app_state_db = make_shared<DBConnector>("APPL_STATE_DB", 0);
            m_counters_db  = make_shared<DBConnector>("COUNTERS_DB",   0);

            sai_attribute_t attr;
            attr.id = SAI_SWITCH_ATTR_INIT_SWITCH;
            attr.value.booldata = true;
            ASSERT_EQ(sai_switch_api->create_switch(&gSwitchId, 1, &attr), SAI_STATUS_SUCCESS);

            attr.id = SAI_SWITCH_ATTR_SRC_MAC_ADDRESS;
            ASSERT_EQ(sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr), SAI_STATUS_SUCCESS);
            gMacAddress = attr.value.mac;

            attr.id = SAI_SWITCH_ATTR_DEFAULT_VIRTUAL_ROUTER_ID;
            ASSERT_EQ(sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr), SAI_STATUS_SUCCESS);
            gVirtualRouterId = attr.value.oid;

            gCrmOrch = new CrmOrch(m_config_db.get(), CFG_CRM_TABLE_NAME);

            TableConnector stateDbSwitchTable(m_state_db.get(), "SWITCH_CAPABILITY");
            TableConnector conf_asic_sensors(m_config_db.get(), CFG_ASIC_SENSORS_TABLE_NAME);
            TableConnector app_switch_table(m_app_db.get(), APP_SWITCH_TABLE_NAME);
            vector<TableConnector> switch_tables = { conf_asic_sensors, app_switch_table };
            gSwitchOrch = new SwitchOrch(m_app_db.get(), switch_tables, stateDbSwitchTable);

            vector<string> flex_counter_tables = {
                CFG_FLEX_COUNTER_TABLE_NAME,
                CFG_DEVICE_METADATA_TABLE_NAME
            };
            auto* flexCounterOrch = new FlexCounterOrch(m_config_db.get(), flex_counter_tables);
            gDirectory.set(flexCounterOrch);

            const vector<string> stel_tables = {
                CFG_HIGH_FREQUENCY_TELEMETRY_PROFILE_TABLE_NAME,
                CFG_HIGH_FREQUENCY_TELEMETRY_GROUP_TABLE_NAME
            };
            gHFTOrch = new HFTelOrch(m_config_db.get(), m_state_db.get(), stel_tables);

            const int portsorch_base_pri = 40;
            vector<table_name_with_pri_t> ports_tables = {
                { APP_PORT_TABLE_NAME,        portsorch_base_pri + 5 },
                { APP_VLAN_TABLE_NAME,        portsorch_base_pri + 2 },
                { APP_VLAN_MEMBER_TABLE_NAME, portsorch_base_pri     },
                { APP_LAG_TABLE_NAME,         portsorch_base_pri + 4 },
                { APP_LAG_MEMBER_TABLE_NAME,  portsorch_base_pri     },
            };
            gPortsOrch = new PortsOrch(m_app_db.get(), m_state_db.get(),
                                       ports_tables, m_chassis_app_db.get());

            gVrfOrch = new VRFOrch(m_app_db.get(), APP_VRF_TABLE_NAME,
                                   m_state_db.get(), STATE_VRF_OBJECT_TABLE_NAME);

            vector<table_name_with_pri_t> intf_tables = {
                { APP_INTF_TABLE_NAME, IntfsOrch::intfsorch_pri },
                { APP_SAG_TABLE_NAME,  IntfsOrch::intfsorch_pri }
            };
            gIntfsOrch = new IntfsOrch(m_app_db.get(), intf_tables,
                                       gVrfOrch, m_chassis_app_db.get());

            vector<table_name_with_pri_t> app_fdb_tables = {
                { APP_FDB_TABLE_NAME,       FdbOrch::fdborch_pri },
                { APP_VXLAN_FDB_TABLE_NAME, FdbOrch::fdborch_pri },
                { APP_MCLAG_FDB_TABLE_NAME, 20 }
            };
            TableConnector stateDbFdb(m_state_db.get(), STATE_FDB_TABLE_NAME);
            TableConnector stateMclagDbFdb(m_state_db.get(), STATE_MCLAG_REMOTE_FDB_TABLE_NAME);
            gFdbOrch = new FdbOrch(m_app_db.get(), app_fdb_tables,
                                   stateDbFdb, stateMclagDbFdb,
                                   gPortsOrch, m_config_db.get());

            gNeighOrch = new NeighOrch(m_app_db.get(), APP_NEIGH_TABLE_NAME,
                                       gIntfsOrch, gFdbOrch, gPortsOrch,
                                       m_chassis_app_db.get());

            vector<string> qos_tables = {
                CFG_TC_TO_QUEUE_MAP_TABLE_NAME, CFG_SCHEDULER_TABLE_NAME,
                CFG_DSCP_TO_TC_MAP_TABLE_NAME,  CFG_MPLS_TC_TO_TC_MAP_TABLE_NAME,
                CFG_DOT1P_TO_TC_MAP_TABLE_NAME, CFG_QUEUE_TABLE_NAME,
                CFG_PORT_QOS_MAP_TABLE_NAME,     CFG_WRED_PROFILE_TABLE_NAME,
                CFG_TC_TO_PRIORITY_GROUP_MAP_TABLE_NAME,
                CFG_PFC_PRIORITY_TO_PRIORITY_GROUP_MAP_TABLE_NAME,
                CFG_PFC_PRIORITY_TO_QUEUE_MAP_TABLE_NAME,
                CFG_DSCP_TO_FC_MAP_TABLE_NAME,  CFG_EXP_TO_FC_MAP_TABLE_NAME
            };
            gQosOrch = new QosOrch(m_config_db.get(), qos_tables);

            // Bring up all SAI ports so that buffer PG/queue events can be applied
            Table portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);
            auto ports = ut_helper::getInitialSaiPorts();
            for (const auto& it : ports)
                portTable.set(it.first, it.second);
            portTable.set("PortConfigDone", { { "count", to_string(ports.size()) } });
            gPortsOrch->addExistingData(&portTable);
            static_cast<Orch*>(gPortsOrch)->doTask();
            portTable.set("PortInitDone", { { "lanes", "0" } });
            gPortsOrch->addExistingData(&portTable);
            static_cast<Orch*>(gPortsOrch)->doTask();
        }

        // Create a BufferOrch backed by config already loaded into CONFIG_DB/APPL_DB.
        BufferOrch* makeBufferOrch()
        {
            vector<string> buffer_tables = {
                APP_BUFFER_POOL_TABLE_NAME,
                APP_BUFFER_PROFILE_TABLE_NAME,
                APP_BUFFER_QUEUE_TABLE_NAME,
                APP_BUFFER_PG_TABLE_NAME,
                APP_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME,
                APP_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME
            };
            return new BufferOrch(m_app_db.get(), m_config_db.get(),
                                  m_state_db.get(), buffer_tables);
        }

        void TearDown() override
        {
            auto buffer_maps = BufferOrch::m_buffer_type_maps;
            for (auto& i : buffer_maps) i.second->clear();

            auto* flexCounterOrch = gDirectory.get<FlexCounterOrch*>();
            if (flexCounterOrch) delete flexCounterOrch;
            gDirectory.m_values.clear();

            if (gBufferOrch) { delete gBufferOrch; gBufferOrch = nullptr; }
            delete gQosOrch;       gQosOrch = nullptr;
            delete gNeighOrch;     gNeighOrch = nullptr;
            delete gFdbOrch;       gFdbOrch = nullptr;
            delete gIntfsOrch;     gIntfsOrch = nullptr;
            delete gVrfOrch;       gVrfOrch = nullptr;
            delete gPortsOrch;     gPortsOrch = nullptr;
            delete gSwitchOrch;    gSwitchOrch = nullptr;
            delete gCrmOrch;       gCrmOrch = nullptr;
            ut_helper::uninitSaiApi();
        }
    };

    // -----------------------------------------------------------------------
    // areAllPortsReady() — empty m_ready_list (no buffer config)
    // -----------------------------------------------------------------------
    TEST_F(SwssReadinessTest, AreAllPortsReady_NoBufferConfig_ReturnsTrue)
    {
        // No CONFIG_DB buffer PG/queue entries → m_ready_list is empty.
        gBufferOrch = makeBufferOrch();

        // With an empty ready list there is nothing pending: must return true.
        EXPECT_TRUE(gBufferOrch->areAllPortsReady());
        EXPECT_TRUE(gBufferOrch->m_ready_list.empty());
    }

    // -----------------------------------------------------------------------
    // areAllPortsReady() — all entries pre-set true
    // -----------------------------------------------------------------------
    TEST_F(SwssReadinessTest, AreAllPortsReady_AllEntriesTrue_ReturnsTrue)
    {
        gBufferOrch = makeBufferOrch();

        // Directly insert already-true entries (simulates processed config).
        gBufferOrch->m_ready_list["Ethernet0|0"] = true;
        gBufferOrch->m_ready_list["Ethernet0|3"] = true;
        gBufferOrch->m_ready_list["Ethernet4|0"] = true;

        EXPECT_TRUE(gBufferOrch->areAllPortsReady());
    }

    // -----------------------------------------------------------------------
    // areAllPortsReady() — one entry still false
    // -----------------------------------------------------------------------
    TEST_F(SwssReadinessTest, AreAllPortsReady_OnePending_ReturnsFalse)
    {
        gBufferOrch = makeBufferOrch();

        gBufferOrch->m_ready_list["Ethernet0|0"] = true;
        gBufferOrch->m_ready_list["Ethernet0|3"] = false; // still pending
        gBufferOrch->m_ready_list["Ethernet4|0"] = true;

        EXPECT_FALSE(gBufferOrch->areAllPortsReady());
    }

    // -----------------------------------------------------------------------
    // areAllPortsReady() — multiple entries false
    // -----------------------------------------------------------------------
    TEST_F(SwssReadinessTest, AreAllPortsReady_MultiplePending_ReturnsFalse)
    {
        gBufferOrch = makeBufferOrch();

        for (int i = 0; i < 8; i++)
            gBufferOrch->m_ready_list["Ethernet0|" + to_string(i)] = false;

        EXPECT_FALSE(gBufferOrch->areAllPortsReady());
    }

    // -----------------------------------------------------------------------
    // areAllPortsReady() — transitions from false to true
    // -----------------------------------------------------------------------
    TEST_F(SwssReadinessTest, AreAllPortsReady_TransitionPendingToReady)
    {
        gBufferOrch = makeBufferOrch();

        gBufferOrch->m_ready_list["Ethernet0|0"] = false;
        gBufferOrch->m_ready_list["Ethernet0|3"] = false;

        EXPECT_FALSE(gBufferOrch->areAllPortsReady());

        // Simulate SAI completion flipping entries to true.
        gBufferOrch->m_ready_list["Ethernet0|0"] = true;
        EXPECT_FALSE(gBufferOrch->areAllPortsReady()); // still one pending

        gBufferOrch->m_ready_list["Ethernet0|3"] = true;
        EXPECT_TRUE(gBufferOrch->areAllPortsReady());  // now all done
    }

    // -----------------------------------------------------------------------
    // areAllPortsReady() — via CONFIG_DB pre-population and APPL_DB processing
    // -----------------------------------------------------------------------
    TEST_F(SwssReadinessTest, AreAllPortsReady_ConfigDbPopulatesReadyList)
    {
        // Write a buffer PG entry to CONFIG_DB before creating BufferOrch.
        // initBufferReadyList reads this at construction time and sets
        // m_ready_list["Ethernet0|0"] = false.
        auto ports = ut_helper::getInitialSaiPorts();
        ASSERT_FALSE(ports.empty());
        string firstPort = ports.begin()->first;

        Table cfgPgTable(m_config_db.get(), CFG_BUFFER_PG_TABLE_NAME);
        cfgPgTable.set(firstPort + config_db_key_delimiter + "0",
                       { { "profile", "ingress_lossy_profile" } });

        gBufferOrch = makeBufferOrch();

        // m_ready_list should have the entry as false (not yet processed).
        string key = firstPort + delimiter + "0";
        ASSERT_NE(gBufferOrch->m_ready_list.find(key),
                  gBufferOrch->m_ready_list.end())
            << "m_ready_list should be pre-populated from CONFIG_DB";
        EXPECT_FALSE(gBufferOrch->m_ready_list.at(key));

        // areAllPortsReady must be false until the APPL_DB event is processed.
        EXPECT_FALSE(gBufferOrch->areAllPortsReady());
    }

    // -----------------------------------------------------------------------
    // areAllPortsReady() — single false entry among many true
    // -----------------------------------------------------------------------
    TEST_F(SwssReadinessTest, AreAllPortsReady_LastEntryPending_ReturnsFalse)
    {
        gBufferOrch = makeBufferOrch();

        // 100 entries all true, last one false.
        for (int i = 0; i < 100; i++)
            gBufferOrch->m_ready_list["Ethernet" + to_string(i * 4) + "|0"] = true;

        gBufferOrch->m_ready_list["Ethernet400|0"] = false; // the last one

        EXPECT_FALSE(gBufferOrch->areAllPortsReady());

        gBufferOrch->m_ready_list["Ethernet400|0"] = true;
        EXPECT_TRUE(gBufferOrch->areAllPortsReady());
    }

    // -----------------------------------------------------------------------
    // up_status written to STATE_DB once PORT_CONFIG_DONE + buffer ready
    // -----------------------------------------------------------------------
    TEST_F(SwssReadinessTest, OrchDaemon_UpStatus_WrittenWhenReady)
    {
        gBufferOrch = makeBufferOrch();

        // Verify gPortsOrch reports config done after SetUp.
        EXPECT_TRUE(gPortsOrch->isConfigDone());

        // With no buffer config, areAllPortsReady() is immediately true.
        EXPECT_TRUE(gBufferOrch->areAllPortsReady());

        // Simulate what orchdaemon does: write up_status when both gates pass.
        Table stateFeatureTable(m_state_db.get(), "FEATURE");
        if (gPortsOrch->isConfigDone() && gBufferOrch->areAllPortsReady())
        {
            stateFeatureTable.hset("swss", "up_status", "true");
        }

        string upStatus;
        EXPECT_TRUE(stateFeatureTable.hget("swss", "up_status", upStatus));
        EXPECT_EQ(upStatus, "true");
    }

    // -----------------------------------------------------------------------
    // up_status NOT written when buffer config is still pending
    // -----------------------------------------------------------------------
    TEST_F(SwssReadinessTest, OrchDaemon_UpStatus_NotWrittenWhenBufferPending)
    {
        gBufferOrch = makeBufferOrch();

        // Inject a pending entry.
        gBufferOrch->m_ready_list["Ethernet0|0"] = false;

        EXPECT_TRUE(gPortsOrch->isConfigDone());
        EXPECT_FALSE(gBufferOrch->areAllPortsReady());

        // Orchdaemon gate: should NOT write up_status.
        Table stateFeatureTable(m_state_db.get(), "FEATURE");
        bool wouldSignal = gPortsOrch->isConfigDone() &&
                           gBufferOrch->areAllPortsReady();
        EXPECT_FALSE(wouldSignal);

        string upStatus;
        EXPECT_FALSE(stateFeatureTable.hget("swss", "up_status", upStatus));
    }

} // namespace swss_readiness_test
