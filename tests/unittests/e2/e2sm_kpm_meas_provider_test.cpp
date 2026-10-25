// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "common/e2ap_asn1_packer.h"
#include "lib/e2/e2sm/e2sm_kpm/e2sm_kpm_cu_meas_provider_impl.h"
#include "lib/e2/e2sm/e2sm_kpm/e2sm_kpm_du_meas_provider_impl.h"
#include "tests/unittests/e2/common/e2_test_helpers.h"
#include "ocudu/ran/du_types.h"
#include "ocudu/support/executors/task_worker.h"
#include "ocudu/support/ocudu_test.h"
#include <gtest/gtest.h>
#include <fstream>

using namespace ocudu;

// Helper global variables to pass pcap_writer to all tests.
static bool      g_enable_pcap = false;
static dlt_pcap* g_pcap        = nullptr;

class e2_rlc_metrics_notifier : public e2_du_metrics_notifier, public e2_du_metrics_interface
{
public:
  void report_metrics(const scheduler_cell_metrics& metrics) override
  {
    if (e2_meas_provider) {
      e2_meas_provider->report_metrics(metrics);
    }
  }

  void report_metrics(const rlc_metrics& metrics) override
  {
    if (e2_meas_provider) {
      e2_meas_provider->report_metrics(metrics);
    }
  }

  void connect_e2_du_meas_provider(std::unique_ptr<e2_du_metrics_notifier> meas_provider) override {}

  void connect_e2_du_meas_provider(e2_du_metrics_notifier* meas_provider) { e2_meas_provider = meas_provider; }

private:
  e2_du_metrics_notifier* e2_meas_provider;
};

class e2_pdcp_metrics_notifier : public e2_cu_metrics_notifier, public e2_cu_metrics_interface
{
public:
  void report_metrics(const pdcp_metrics_container& metrics) override
  {
    if (e2_meas_provider) {
      e2_meas_provider->report_metrics(metrics);
    }
  }

  void report_metrics(const ocuup::f1u_metrics_container& metrics) override
  {
    if (e2_meas_provider) {
      e2_meas_provider->report_metrics(metrics);
    }
  }

  void report_metrics(const cu_cp_metrics_report& metrics) override
  {
    if (e2_meas_provider) {
      e2_meas_provider->report_metrics(metrics);
    }
  }

  void connect_e2_cu_meas_provider(std::unique_ptr<e2_cu_metrics_notifier> meas_provider) override {}

  void connect_e2_cu_meas_provider(e2_cu_metrics_notifier* meas_provider) { e2_meas_provider = meas_provider; }

private:
  e2_cu_metrics_notifier* e2_meas_provider;
};

class e2_entity_test_with_pcap : public e2_test_base_with_pcap
{
protected:
  dlt_pcap*                                           external_pcap_writer;
  std::unique_ptr<e2_node_component_config_collector> node_component_config_collector;

  void SetUp() override
  {
    external_pcap_writer = GetParam();

    ocudulog::fetch_basic_logger("TEST").set_level(ocudulog::basic_levels::debug);
    ocudulog::init();

    cfg                  = config_helpers::make_default_e2ap_config();
    cfg.e2sm_kpm_enabled = true;

    gw                              = std::make_unique<dummy_sctp_association_sdu_notifier>();
    pcap                            = std::make_unique<dummy_e2ap_pcap>();
    e2_client                       = std::make_unique<dummy_e2_connection_client>();
    du_metrics                      = std::make_unique<dummy_e2_du_metrics>();
    f1ap_ue_id_mapper               = std::make_unique<dummy_f1ap_ue_id_translator>();
    factory                         = timer_factory{timers, task_worker};
    du_rc_param_configurator        = std::make_unique<dummy_du_configurator>();
    node_component_config_collector = std::make_unique<e2_node_component_config_collector>(task_worker, 1);
    // Pre-deliver a dummy F1 config so the setup coroutine is not blocked awaiting it.
    node_component_config_collector->deliver(e2_node_component_interface_type::f1, {}, {});
    e2agent = create_e2_du_agent(cfg,
                                 *e2_client,
                                 du_metrics.get(),
                                 f1ap_ue_id_mapper.get(),
                                 du_rc_param_configurator.get(),
                                 factory,
                                 task_worker,
                                 std::move(node_component_config_collector));
    task_worker.run_pending_tasks();
    if (external_pcap_writer) {
      packer = std::make_unique<e2ap_asn1_packer>(*gw, e2agent->get_e2_interface(), *external_pcap_writer);
    } else {
      packer = std::make_unique<e2ap_asn1_packer>(*gw, e2agent->get_e2_interface(), *pcap);
    }
  }

  void TearDown() override
  {
    // flush logger after each test
    ocudulog::flush();
    pcap->close();
  }
};

class e2sm_kpm_meas_provider_test : public testing::TestWithParam<dlt_pcap*>
{
protected:
  dlt_pcap* external_pcap_writer;

  void SetUp() override
  {
    external_pcap_writer = GetParam();

    ocudulog::fetch_basic_logger("TEST").set_level(ocudulog::basic_levels::debug);
    ocudulog::init();

    cfg                  = config_helpers::make_default_e2ap_config();
    cfg.e2sm_kpm_enabled = true;
    gw                   = std::make_unique<dummy_sctp_association_sdu_notifier>();
    pcap                 = std::make_unique<dummy_e2ap_pcap>();
    e2                   = std::make_unique<dummy_e2_interface>();
    if (external_pcap_writer) {
      packer = std::make_unique<e2ap_asn1_packer>(*gw, *e2, *external_pcap_writer);
    } else {
      packer = std::make_unique<e2ap_asn1_packer>(*gw, *e2, *pcap);
    }
  }

  void TearDown() override
  {
    // Flush logger after each test.
    ocudulog::flush();
    pcap->close();
  }

  void tick()
  {
    timers.tick();
    task_worker.run_pending_tasks();
  }
  e2ap_configuration                                   cfg = {};
  timer_factory                                        factory;
  timer_manager                                        timers;
  std::unique_ptr<dummy_sctp_association_sdu_notifier> gw;
  std::unique_ptr<e2_interface>                        e2;
  std::unique_ptr<dummy_e2ap_pcap>                     pcap;
  std::unique_ptr<e2ap_asn1_packer>                    packer;
  std::unique_ptr<e2sm_interface>                      e2sm_iface;
  std::unique_ptr<e2sm_handler>                        e2sm_packer;
  std::unique_ptr<e2_subscription_manager>             e2_subscription_mngr;
  manual_task_worker                                   task_worker{64};
  std::unique_ptr<dummy_e2_pdu_notifier>               msg_notifier;
  std::unique_ptr<dummy_e2_connection_client>          e2_client;
  ocudulog::basic_logger&                              test_logger = ocudulog::fetch_basic_logger("TEST");
};

class e2sm_kpm_du_meas_provider_test : public e2sm_kpm_meas_provider_test
{
  void SetUp() override
  {
    e2sm_kpm_meas_provider_test::SetUp();
    f1ap_ue_id_mapper = std::make_unique<dummy_f1ap_ue_id_translator>();
    du_meas_provider  = std::make_unique<e2sm_kpm_du_meas_provider_impl>(*f1ap_ue_id_mapper, 30);
    e2sm_packer       = std::make_unique<e2sm_kpm_asn1_packer>(*du_meas_provider);
    e2sm_iface        = std::make_unique<e2sm_kpm_impl>(test_logger, *e2sm_packer, *du_meas_provider);
    metrics           = std::make_unique<e2_rlc_metrics_notifier>();
    metrics->connect_e2_du_meas_provider(du_meas_provider.get());
  }

  void TearDown() override { e2sm_kpm_meas_provider_test::TearDown(); }

protected:
  std::unique_ptr<e2_rlc_metrics_notifier>        metrics;
  std::unique_ptr<dummy_f1ap_ue_id_translator>    f1ap_ue_id_mapper;
  std::unique_ptr<e2sm_kpm_du_meas_provider_impl> du_meas_provider;
};

class e2sm_kpm_cu_meas_provider_test : public e2sm_kpm_meas_provider_test
{
  void SetUp() override
  {
    e2sm_kpm_meas_provider_test::SetUp();
    cu_meas_provider = std::make_unique<e2sm_kpm_cu_up_meas_provider_impl>();
    e2sm_packer      = std::make_unique<e2sm_kpm_asn1_packer>(*cu_meas_provider);
    e2sm_iface       = std::make_unique<e2sm_kpm_impl>(test_logger, *e2sm_packer, *cu_meas_provider);
    metrics          = std::make_unique<e2_pdcp_metrics_notifier>();
    metrics->connect_e2_cu_meas_provider(cu_meas_provider.get());
  }

  void TearDown() override { e2sm_kpm_meas_provider_test::TearDown(); }

protected:
  std::unique_ptr<e2_pdcp_metrics_notifier>          metrics;
  std::unique_ptr<e2sm_kpm_cu_up_meas_provider_impl> cu_meas_provider;
};

static rlc_metrics generate_rlc_metrics(uint32_t ue_idx, uint32_t bearer_id)
{
  rlc_metrics rlc_metric;
  rlc_metric.metrics_period        = std::chrono::milliseconds(1000);
  rlc_metric.ue_index              = static_cast<du_ue_index_t>(ue_idx);
  rlc_metric.rb_id                 = rb_id_t(drb_id_t(bearer_id));
  rlc_metric.rx.mode_specific      = rlc_am_rx_metrics{};
  rlc_metric.rx.num_pdus           = 5;
  rlc_metric.rx.num_pdu_bytes      = rlc_metric.rx.num_pdus * 1000;
  rlc_metric.rx.num_sdus           = 5;
  rlc_metric.rx.num_sdu_bytes      = rlc_metric.rx.num_sdus * 1000;
  rlc_metric.rx.num_lost_pdus      = 1;
  rlc_metric.rx.num_malformed_pdus = 0;

  rlc_metric.tx.tx_high.num_sdus                     = 10;
  rlc_metric.tx.tx_high.num_sdu_bytes                = rlc_metric.tx.tx_high.num_sdus * 1000;
  rlc_metric.tx.tx_high.num_dropped_sdus             = 1;
  rlc_metric.tx.tx_high.num_discarded_sdus           = 0;
  rlc_metric.tx.tx_high.num_discard_failures         = 0;
  rlc_metric.tx.tx_low.num_pdus_no_segmentation      = 8;
  rlc_metric.tx.tx_low.num_pdu_bytes_no_segmentation = rlc_metric.tx.tx_low.num_pdus_no_segmentation * 1000;

  rlc_metric.tx.tx_low.mode_specific = rlc_am_tx_metrics_lower{};
  auto& am                           = std::get<rlc_am_tx_metrics_lower>(rlc_metric.tx.tx_low.mode_specific);
  am.num_pdus_with_segmentation      = 2;
  am.num_pdu_bytes_with_segmentation = am.num_pdus_with_segmentation * 1000;

  return rlc_metric;
}

static scheduler_cell_metrics generate_sched_metrics(uint32_t                           nof_prbs,
                                                     uint32_t                           nof_slots,
                                                     std::vector<std::vector<uint32_t>> dl_grants,
                                                     std::vector<std::vector<uint32_t>> ul_grants)
{
  scheduler_cell_metrics sched_metric;
  sched_metric.nof_prbs     = nof_prbs;
  sched_metric.nof_dl_slots = nof_slots;
  sched_metric.nof_ul_slots = nof_slots;

  assert(dl_grants[0].size() == nof_slots);
  assert(ul_grants[0].size() == nof_slots);

  for (uint32_t ue_idx = 0; ue_idx < nof_slots; ++ue_idx) {
    scheduler_ue_metrics ue_metrics;
    ue_metrics.ue_index = to_du_ue_index(0);
    ue_metrics.pci      = 1;
    ue_metrics.rnti     = static_cast<rnti_t>(0x1000 + ue_idx);
    ue_metrics.tot_pdsch_prbs_used =
        (ue_idx < dl_grants.size()) ? std::accumulate(dl_grants[ue_idx].begin(), dl_grants[ue_idx].end(), 0) : 0;
    ue_metrics.tot_pusch_prbs_used =
        (ue_idx < ul_grants.size()) ? std::accumulate(ul_grants[ue_idx].begin(), ul_grants[ue_idx].end(), 0) : 0;
    sched_metric.ue_metrics.push_back(ue_metrics);
  }

  return sched_metric;
}

// E2 Setup Request is needed for Wireshark to correctly decode the subsequent Subscription Requests
TEST_P(e2_entity_test_with_pcap, e2sm_kpm_generates_ran_func_desc)
{
  // We need this test to generate E2 Setup Request, so Wireshark can decode the following RIC indication messages.
  test_logger.info("Launch e2 setup request procedure with task worker...");
  e2agent->start();

  // Save E2 Setup Request
  packer->handle_message(e2_client->last_tx_e2_pdu);

  // Need to send setup response, so the transaction can be completed.
  unsigned   transaction_id    = get_transaction_id(e2_client->last_tx_e2_pdu.pdu).value();
  e2_message e2_setup_response = generate_e2_setup_response(transaction_id);
  e2_setup_response.pdu.successful_outcome()
      .value.e2setup_resp()
      ->ran_functions_accepted[0]
      ->ran_function_id_item()
      .ran_function_id = e2sm_kpm_asn1_packer::ran_func_id;
  test_logger.info("Injecting E2SetupResponse");
  e2agent->get_e2_interface().handle_message(e2_setup_response);
  e2agent->stop();
}

TEST_P(e2sm_kpm_du_meas_provider_test, e2sm_kpm_ind_three_drb_rlc_metrics)
{
  std::vector<uint32_t> ue_ids        = {31, 23, 152};
  std::vector<uint32_t> nof_drbs      = {3, 1, 2};
  uint32_t              nof_ues       = ue_ids.size();
  uint32_t              nof_meas_data = 5;
  uint32_t              nof_records   = 1;

  uint32_t              expected_drop_rate     = 10;
  float                 expected_dl_throughput = 10000 / 1e3 * 8;
  float                 expected_ul_throughput = 5000 / 1e3 * 8;
  std::vector<uint32_t> expected_dl_vol;
  std::vector<uint32_t> expected_ul_vol;

  for (auto& d : nof_drbs) {
    // Unit is kbit.
    expected_dl_vol.push_back(10 * 1000 * d * 8 / 1000);
    expected_ul_vol.push_back(5 * 1000 * d * 8 / 1000);
  }

  // Define E2SM_KPM action format 5.
  asn1::e2sm::e2sm_kpm_action_definition_s action_def;
  action_def.ric_style_type = 5;
  asn1::e2sm::e2sm_kpm_action_definition_format5_s& action_def_f5 =
      action_def.action_definition_formats.set_action_definition_format5();

  action_def_f5.matching_ue_id_list.resize(nof_ues);
  for (unsigned i = 0; i < nof_ues; i++) {
    action_def_f5.matching_ue_id_list[i].ue_id.set_gnb_du_ue_id() = generate_ueid_gnb_du(ue_ids[i]);
  }

  asn1::e2sm::e2sm_kpm_action_definition_format1_s& subscript_info = action_def_f5.sub_info;
  subscript_info.cell_global_id_present                            = false;
  subscript_info.granul_period                                     = 100;

  asn1::e2sm::meas_info_item_s meas_info_item;
  meas_info_item.meas_type.set_meas_name().from_string("DRB.RlcPacketDropRateDl");
  asn1::e2sm::label_info_item_s label_info_item{};
  label_info_item.meas_label.no_label_present = true;
  label_info_item.meas_label.no_label         = asn1::e2sm::meas_label_s::no_label_opts::true_value;
  meas_info_item.label_info_list.push_back(label_info_item);
  subscript_info.meas_info_list.push_back(meas_info_item);
  meas_info_item.meas_type.set_meas_name().from_string("DRB.RlcSduTransmittedVolumeDL");
  subscript_info.meas_info_list.push_back(meas_info_item);
  meas_info_item.meas_type.set_meas_name().from_string("DRB.RlcSduTransmittedVolumeUL");
  subscript_info.meas_info_list.push_back(meas_info_item);
  meas_info_item.meas_type.set_meas_name().from_string("DRB.UEThpDl");
  subscript_info.meas_info_list.push_back(meas_info_item);
  meas_info_item.meas_type.set_meas_name().from_string("DRB.UEThpUl");
  subscript_info.meas_info_list.push_back(meas_info_item);

  nof_records = subscript_info.meas_info_list.size();

  asn1::e2ap::ric_action_to_be_setup_item_s ric_action = generate_e2sm_kpm_ric_action(action_def);

  if (g_enable_pcap) {
    // Save E2 Subscription Request.
    e2_message e2_subscript_req = generate_e2sm_kpm_subscription_request(ric_action);
    packer->handle_message(e2_subscript_req);
  }

  ASSERT_TRUE(e2sm_iface->action_supported(ric_action));
  auto report_service = e2sm_iface->get_e2sm_report_service(ric_action.ric_action_definition);

  for (unsigned i = 0; i < nof_meas_data; ++i) {
    // Push dummy metric measurements.
    rlc_metrics rlc_metrics;
    for (unsigned u = 0; u < ue_ids.size(); ++u) {
      for (unsigned b = 1; b < (nof_drbs[u] + 1); ++b) {
        rlc_metrics = generate_rlc_metrics(ue_ids[u], b);
        metrics->report_metrics(rlc_metrics);
      }
    }

    // Trigger measurement collection.
    report_service->collect_measurements();
  }

  TESTASSERT_EQ(true, report_service->is_ind_msg_ready());
  // Get RIC indication msg content.
  byte_buffer ind_hdr_bytes = report_service->get_indication_header();
  byte_buffer ind_msg_bytes = report_service->get_indication_message();

  // Decode RIC Indication and check the content.
  asn1::e2sm::e2sm_kpm_ind_msg_s ric_ind_msg;
  asn1::cbit_ref                 ric_ind_bref(ind_msg_bytes);
  if (ric_ind_msg.unpack(ric_ind_bref) != asn1::OCUDUASN_SUCCESS) {
    test_logger.debug("e2sm_kpm: RIC indication msg could not be unpacked");
    return;
  }

  TESTASSERT_EQ(nof_ues, ric_ind_msg.ind_msg_formats.ind_msg_format3().ue_meas_report_list.size());
  for (unsigned ue_idx = 0; ue_idx < nof_ues; ++ue_idx) {
    TESTASSERT_EQ(
        nof_meas_data,
        ric_ind_msg.ind_msg_formats.ind_msg_format3().ue_meas_report_list[ue_idx].meas_report.meas_data.size());
    for (unsigned i = 0; i < nof_meas_data; ++i) {
      auto& meas_record = ric_ind_msg.ind_msg_formats.ind_msg_format3()
                              .ue_meas_report_list[ue_idx]
                              .meas_report.meas_data[i]
                              .meas_record;
      TESTASSERT_EQ(nof_records, meas_record.size());
      TESTASSERT_EQ(expected_drop_rate, meas_record[0].integer());
      if (nof_records >= 2) {
        TESTASSERT_EQ((i + 1) * expected_dl_vol[ue_idx], meas_record[1].integer());
      }
      if (nof_records >= 3) {
        TESTASSERT_EQ((i + 1) * expected_ul_vol[ue_idx], meas_record[2].integer());
      }
      if (nof_records >= 4) {
        TESTASSERT_EQ(expected_dl_throughput, meas_record[3].real().value);
      }
      if (nof_records >= 5) {
        TESTASSERT_EQ(expected_ul_throughput, meas_record[4].real().value);
      }
    }
  }

  if (g_enable_pcap) {
    e2_message e2_msg = generate_e2_ind_msg(ind_hdr_bytes, ind_msg_bytes);
    packer->handle_message(e2_msg);
  }
}

TEST_P(e2sm_kpm_du_meas_provider_test, e2sm_kpm_ind_e2_level_rlc_metrics)
{
  std::vector<uint32_t> ue_ids        = {31, 23, 152};
  std::vector<uint32_t> nof_drbs      = {3, 1, 2};
  uint32_t              nof_meas_data = 5;
  uint32_t              nof_records   = 3;

  uint32_t expected_drop_rate = 10;
  uint32_t expected_dl_vol    = 0;
  uint32_t expected_ul_vol    = 0;

  for (auto& d : nof_drbs) {
    // Unit is kbit.
    expected_dl_vol += (10 * 1000 * d * 8 / 1000);
    expected_ul_vol += (5 * 1000 * d * 8 / 1000);
  }

  // Define E2SM_KPM action format 1.
  asn1::e2sm::e2sm_kpm_action_definition_s action_def;
  action_def.ric_style_type = 1;
  asn1::e2sm::e2sm_kpm_action_definition_format1_s& action_def_f1 =
      action_def.action_definition_formats.set_action_definition_format1();
  action_def_f1.cell_global_id_present = false;
  action_def_f1.granul_period          = 100;

  asn1::e2sm::meas_info_item_s meas_info_item{};
  meas_info_item.meas_type.set_meas_name().from_string("DRB.RlcPacketDropRateDl"); // Dummy metric not supported.
  asn1::e2sm::label_info_item_s label_info_item{};
  label_info_item.meas_label.no_label_present = true;
  label_info_item.meas_label.no_label         = asn1::e2sm::meas_label_s::no_label_opts::true_value;
  meas_info_item.label_info_list.push_back(label_info_item);
  action_def_f1.meas_info_list.push_back(meas_info_item);
  meas_info_item.meas_type.set_meas_name().from_string("DRB.RlcSduTransmittedVolumeDL");
  action_def_f1.meas_info_list.push_back(meas_info_item);
  meas_info_item.meas_type.set_meas_name().from_string("DRB.RlcSduTransmittedVolumeUL");
  action_def_f1.meas_info_list.push_back(meas_info_item);

  asn1::e2ap::ric_action_to_be_setup_item_s ric_action = generate_e2sm_kpm_ric_action(action_def);

  if (g_enable_pcap) {
    // Save E2 Subscription Request.
    e2_message e2_subscript_req = generate_e2sm_kpm_subscription_request(ric_action);
    packer->handle_message(e2_subscript_req);
  }

  ASSERT_TRUE(e2sm_iface->action_supported(ric_action));
  auto report_service = e2sm_iface->get_e2sm_report_service(ric_action.ric_action_definition);

  for (unsigned i = 0; i < nof_meas_data; ++i) {
    // Push dummy metric measurements.
    rlc_metrics rlc_metrics;
    for (unsigned u = 0; u < ue_ids.size(); ++u) {
      for (unsigned b = 1; b < (nof_drbs[u] + 1); ++b) {
        rlc_metrics = generate_rlc_metrics(ue_ids[u], b);
        metrics->report_metrics(rlc_metrics);
      }
    }

    // Trigger measurement collection.
    report_service->collect_measurements();
  }

  TESTASSERT_EQ(true, report_service->is_ind_msg_ready());
  // Get RIC indication msg content.
  byte_buffer ind_hdr_bytes = report_service->get_indication_header();
  byte_buffer ind_msg_bytes = report_service->get_indication_message();

  // Decode RIC Indication and check the content.
  asn1::e2sm::e2sm_kpm_ind_msg_s ric_ind_msg;
  asn1::cbit_ref                 ric_ind_bref(ind_msg_bytes);
  if (ric_ind_msg.unpack(ric_ind_bref) != asn1::OCUDUASN_SUCCESS) {
    test_logger.debug("e2sm_kpm: RIC indication msg could not be unpacked");
    return;
  }

  TESTASSERT_EQ(nof_meas_data, ric_ind_msg.ind_msg_formats.ind_msg_format1().meas_data.size());
  for (unsigned i = 0; i < nof_meas_data; ++i) {
    auto& meas_record = ric_ind_msg.ind_msg_formats.ind_msg_format1().meas_data[i].meas_record;
    TESTASSERT_EQ(nof_records, meas_record.size());
    TESTASSERT_EQ(expected_drop_rate, meas_record[0].integer());
    TESTASSERT_EQ((i + 1) * expected_dl_vol, meas_record[1].integer());
    TESTASSERT_EQ((i + 1) * expected_ul_vol, meas_record[2].integer());
  }

  if (g_enable_pcap) {
    e2_message e2_msg = generate_e2_ind_msg(ind_hdr_bytes, ind_msg_bytes);
    packer->handle_message(e2_msg);
  }
}

TEST_P(e2sm_kpm_du_meas_provider_test, e2sm_kpm_ind_e2_level_prb_metrics)
{
  std::vector<uint32_t>              grants_ue0 = {1, 0, 3, 4, 5, 6, 7, 8, 9, 0};
  std::vector<uint32_t>              grants_ue1 = {0, 0, 8, 7, 6, 5, 4, 3, 2, 1};
  std::vector<uint32_t>              grants_ue2 = {5, 0, 5, 0, 5, 0, 5, 0, 5, 0};
  std::vector<uint32_t>              grants_ue3 = {0, 0, 0, 0, 0, 5, 5, 5, 5, 5};
  std::vector<std::vector<uint32_t>> dl_grants  = {grants_ue0, grants_ue1, grants_ue2, grants_ue3};
  std::vector<std::vector<uint32_t>> ul_grants  = {grants_ue0, grants_ue1};

  uint32_t nof_prbs      = 25;
  uint32_t nof_slots     = grants_ue0.size();
  uint32_t nof_meas_data = 1;
  uint32_t nof_records   = 6;

  uint32_t tot_dl_prbs = 0;
  uint32_t tot_ul_prbs = 0;
  for (const auto& grants : dl_grants) {
    tot_dl_prbs += std::accumulate(grants.begin(), grants.end(), 0);
  }
  for (const auto& grants : ul_grants) {
    tot_ul_prbs += std::accumulate(grants.begin(), grants.end(), 0);
  }

  uint32_t expected_dl_used_prbs  = tot_dl_prbs / nof_slots;
  uint32_t expected_ul_used_prbs  = tot_ul_prbs / nof_slots;
  uint32_t expected_dl_avail_prbs = nof_prbs - expected_dl_used_prbs;
  uint32_t expected_ul_avail_prbs = nof_prbs - expected_ul_used_prbs;
  // Use all PRB-slot observations; the integer PRB/slot display is already rounded down.
  uint32_t expected_dl_tot_prbs   = uint64_t{100} * tot_dl_prbs / (nof_slots * nof_prbs);
  uint32_t expected_ul_tot_prbs   = uint64_t{100} * tot_ul_prbs / (nof_slots * nof_prbs);

  // Define E2SM_KPM action format 1.
  asn1::e2sm::e2sm_kpm_action_definition_s action_def;
  action_def.ric_style_type = 1;
  asn1::e2sm::e2sm_kpm_action_definition_format1_s& action_def_f1 =
      action_def.action_definition_formats.set_action_definition_format1();
  action_def_f1.cell_global_id_present = false;
  action_def_f1.granul_period          = 100;

  asn1::e2sm::meas_info_item_s  meas_info_item{};
  asn1::e2sm::label_info_item_s label_info_item{};
  label_info_item.meas_label.no_label_present = true;
  label_info_item.meas_label.no_label         = asn1::e2sm::meas_label_s::no_label_opts::true_value;
  meas_info_item.label_info_list.push_back(label_info_item);

  meas_info_item.meas_type.set_meas_name().from_string("RRU.PrbUsedDl");
  action_def_f1.meas_info_list.push_back(meas_info_item);
  meas_info_item.meas_type.set_meas_name().from_string("RRU.PrbUsedUl");
  action_def_f1.meas_info_list.push_back(meas_info_item);
  meas_info_item.meas_type.set_meas_name().from_string("RRU.PrbAvailDl");
  action_def_f1.meas_info_list.push_back(meas_info_item);
  meas_info_item.meas_type.set_meas_name().from_string("RRU.PrbAvailUl");
  action_def_f1.meas_info_list.push_back(meas_info_item);
  meas_info_item.meas_type.set_meas_name().from_string("RRU.PrbTotDl");
  action_def_f1.meas_info_list.push_back(meas_info_item);
  meas_info_item.meas_type.set_meas_name().from_string("RRU.PrbTotUl");
  action_def_f1.meas_info_list.push_back(meas_info_item);

  asn1::e2ap::ric_action_to_be_setup_item_s ric_action = generate_e2sm_kpm_ric_action(action_def);

  if (g_enable_pcap) {
    // Save E2 Subscription Request.
    e2_message e2_subscript_req = generate_e2sm_kpm_subscription_request(ric_action);
    packer->handle_message(e2_subscript_req);
  }

  ASSERT_TRUE(e2sm_iface->action_supported(ric_action));
  auto report_service = e2sm_iface->get_e2sm_report_service(ric_action.ric_action_definition);

  // Push dummy metric measurements.
  scheduler_cell_metrics sched_metrics;
  sched_metrics = generate_sched_metrics(nof_prbs, nof_slots, dl_grants, ul_grants);
  metrics->report_metrics(sched_metrics);

  // Trigger measurement collection.
  report_service->collect_measurements();

  TESTASSERT_EQ(true, report_service->is_ind_msg_ready());
  // Get RIC indication msg content.
  byte_buffer ind_hdr_bytes = report_service->get_indication_header();
  byte_buffer ind_msg_bytes = report_service->get_indication_message();

  // Decode RIC Indication and check the content.
  asn1::e2sm::e2sm_kpm_ind_msg_s ric_ind_msg;
  asn1::cbit_ref                 ric_ind_bref(ind_msg_bytes);
  if (ric_ind_msg.unpack(ric_ind_bref) != asn1::OCUDUASN_SUCCESS) {
    test_logger.debug("e2sm_kpm: RIC indication msg could not be unpacked");
    return;
  }

  TESTASSERT_EQ(nof_meas_data, ric_ind_msg.ind_msg_formats.ind_msg_format1().meas_data.size());
  auto& meas_record = ric_ind_msg.ind_msg_formats.ind_msg_format1().meas_data[0].meas_record;
  TESTASSERT_EQ(nof_records, meas_record.size());
  TESTASSERT_EQ(expected_dl_used_prbs, meas_record[0].integer());
  TESTASSERT_EQ(expected_ul_used_prbs, meas_record[1].integer());
  TESTASSERT_EQ(expected_dl_avail_prbs, meas_record[2].integer());
  TESTASSERT_EQ(expected_ul_avail_prbs, meas_record[3].integer());
  TESTASSERT_EQ(expected_dl_tot_prbs, meas_record[4].integer());
  TESTASSERT_EQ(expected_ul_tot_prbs, meas_record[5].integer());

  if (g_enable_pcap) {
    e2_message e2_msg = generate_e2_ind_msg(ind_hdr_bytes, ind_msg_bytes);
    packer->handle_message(e2_msg);
  }
}

TEST(cu_identity, standard_ueid_contains_amf_and_f1_and_real_state)
{
  e2sm_kpm_cu_cp_meas_provider_impl provider;
  cu_cp_metrics_report report;
  for (unsigned i = 0; i < 2; ++i) {
    cu_cp_metrics_report::ue_info ue{};
    ue.du_id = int_to_gnb_du_id(i + 1);
    ue.ue_index = static_cast<ocucp::ue_index_t>(42 + i);
    ue.rrc_connection_state = ocucp::rrc_state::connected;
    guami_t guami{};
    guami.plmn = plmn_identity::parse("00101").value();
    guami.amf_region_id = 2;
    guami.amf_set_id = 1;
    guami.amf_pointer = 0;
    ue.identity = ocucp::cu_ue_identity{{(1ULL << 39) + i, 301 + i, guami}, {1007 + i, 8 + i}, ue.du_id};
    ue.slice_ssts = {1};
    report.ues.push_back(ue);
  }
  provider.report_metrics(report);
  std::vector<asn1::e2sm::ue_id_c> ids;
  asn1::e2sm::matching_ue_cond_per_sub_list_l conditions;
  conditions.resize(1);
  auto& condition = conditions[0].test_cond_info;
  condition.test_type.set_snssai();
  condition.test_expr_present = true;
  condition.test_expr = asn1::e2sm::test_cond_expression_e::equal;
  condition.test_value_present = true;
  condition.test_value.set_value_int() = 1;
  ASSERT_TRUE(provider.get_ues_matching_test_conditions(conditions, ids));
  ASSERT_EQ(ids.size(), 2U);
  ASSERT_EQ(ids[0].gnb_ue_id().amf_ue_ngap_id, 1ULL << 39);
  ASSERT_EQ(ids[0].gnb_ue_id().gnb_cu_ue_f1ap_id_list[0].gnb_cu_ue_f1ap_id, 1007U);
  ASSERT_TRUE(ids[0].gnb_ue_id().ran_ue_id_present);
  ASSERT_EQ(ids[0].gnb_ue_id().ran_ue_id.to_number(), (uint64_t{1} << 28) | 42U);
  ASSERT_EQ(ids[1].gnb_ue_id().ran_ue_id.to_number(), (uint64_t{2} << 28) | 43U);
  ASSERT_TRUE(provider.is_ue_supported(ids[0]));

  asn1::e2sm::label_info_list_l labels;
  labels.resize(1);
  labels[0].meas_label.no_label_present = true;
  labels[0].meas_label.no_label = asn1::e2sm::meas_label_s::no_label_e_::true_value;
  asn1::e2sm::meas_type_c metric;
  metric.set_meas_name().from_string("FALCON.UE.RRCState");
  std::vector<asn1::e2sm::meas_record_item_c> values;
  ASSERT_TRUE(provider.get_meas_data(metric, labels, ids, {}, values));
  ASSERT_EQ(values.size(), 2U);
  ASSERT_EQ(values[0].integer(), 1U);

  // Encode actual OCUDU ASN.1; optionally export to verify an independent FlexRIC decoder.
  asn1::e2sm::e2sm_kpm_ind_msg_s message;
  auto& rows = message.ind_msg_formats.set_ind_msg_format3().ue_meas_report_list;
  rows.resize(2);
  for (unsigned i = 0; i < 2; ++i) {
    rows[i].ue_id = ids[i];
    auto& measurement = rows[i].meas_report;
    measurement.meas_info_list.resize(1);
    measurement.meas_info_list[0].meas_type = metric;
    measurement.meas_info_list[0].label_info_list = labels;
    measurement.meas_data.resize(1);
    measurement.meas_data[0].meas_record.push_back(values[i]);
    measurement.granul_period_present = true;
    measurement.granul_period = 1000;
  }
  byte_buffer encoded;
  asn1::bit_ref writer(encoded);
  ASSERT_EQ(message.pack(writer), asn1::OCUDUASN_SUCCESS);
  asn1::e2sm::e2sm_kpm_ind_msg_s decoded;
  asn1::cbit_ref reader(encoded);
  ASSERT_EQ(decoded.unpack(reader), asn1::OCUDUASN_SUCCESS);
  ASSERT_EQ(decoded.ind_msg_formats.ind_msg_format3().ue_meas_report_list[1].ue_id.gnb_ue_id().amf_ue_ngap_id,
            (1ULL << 39) + 1);
  if (const char* directory = std::getenv("FALCON_IDENTITY_FIXTURE_DIR")) {
    std::ofstream msg(std::string(directory) + "/cu_message.asn", std::ios::binary);
    for (auto b : encoded) msg.put(static_cast<char>(b));
    ASSERT_TRUE(msg.good());
    asn1::e2sm::e2sm_kpm_ind_hdr_s header;
    header.ind_hdr_formats.ind_hdr_format1().collet_start_time.from_number(2208988900ULL << 32);
    byte_buffer header_bytes;
    asn1::bit_ref header_writer(header_bytes);
    ASSERT_EQ(header.pack(header_writer), asn1::OCUDUASN_SUCCESS);
    std::ofstream hdr(std::string(directory) + "/cu_header.asn", std::ios::binary);
    for (auto b : header_bytes) hdr.put(static_cast<char>(b));
    ASSERT_TRUE(hdr.good());
  }

  // A different requested slice cannot manufacture identities.
  condition.test_value.set_value_int() = 2;
  ASSERT_TRUE(provider.get_ues_matching_test_conditions(conditions, ids));
  EXPECT_TRUE(ids.empty());
  // Retirement invalidates the real state, even for an old subscribed UEID.
  report.ues.clear();
  provider.report_metrics(report);
  values.clear();
  std::vector<asn1::e2sm::ue_id_c> old_ids = {rows[0].ue_id};
  ASSERT_TRUE(provider.get_meas_data(metric, labels, old_ids, {}, values));
  EXPECT_EQ(values[0].type(), asn1::e2sm::meas_record_item_c::types::no_value);
  EXPECT_FALSE(provider.is_ue_supported(old_ids[0]));
}

TEST(cu_identity, style4_subscription_repeated_collection_preserves_ue_identity)
{
  e2sm_kpm_cu_cp_meas_provider_impl provider;
  e2sm_kpm_asn1_packer packer(provider);
  e2sm_kpm_impl service(ocudulog::fetch_basic_logger("TEST"), packer, provider);
  cu_cp_metrics_report report;
  cu_cp_metrics_report::ue_info ue{};
  ue.rrc_connection_state = ocucp::rrc_state::connected;
  ue.ue_index = static_cast<ocucp::ue_index_t>(7);
  guami_t guami{};
  guami.plmn = plmn_identity::parse("00101").value();
  ue.identity = ocucp::cu_ue_identity{{0, 301, guami}, {1007, 9}, int_to_gnb_du_id(2)};
  ue.slice_ssts = {1};
  report.ues.push_back(ue);
  provider.report_metrics(report);

  asn1::e2sm::e2sm_kpm_action_definition_s action;
  action.ric_style_type = 4;
  auto& format = action.action_definition_formats.set_action_definition_format4();
  format.matching_ue_cond_list.resize(1);
  auto& condition = format.matching_ue_cond_list[0].test_cond_info;
  condition.test_type.set_snssai();
  condition.test_expr_present = true;
  condition.test_expr = asn1::e2sm::test_cond_expression_e::equal;
  condition.test_value_present = true;
  condition.test_value.set_value_int() = 1;
  format.sub_info.granul_period = 1000;
  format.sub_info.meas_info_list.resize(1);
  auto& metric = format.sub_info.meas_info_list[0];
  metric.meas_type.set_meas_name().from_string("FALCON.UE.RRCState");
  metric.label_info_list.resize(1);
  metric.label_info_list[0].meas_label.no_label_present = true;
  metric.label_info_list[0].meas_label.no_label = asn1::e2sm::meas_label_s::no_label_e_::true_value;
  auto subscription = generate_e2sm_kpm_ric_action(action);
  ASSERT_TRUE(service.action_supported(subscription));
  EXPECT_FALSE(provider.is_metric_supported(metric.meas_type, metric.label_info_list[0].meas_label, E2_NODE_LEVEL, false));
  auto collector = service.get_e2sm_report_service(subscription.ric_action_definition);
  ASSERT_NE(collector, nullptr);
  collector->collect_measurements();
  collector->collect_measurements();
  ASSERT_TRUE(collector->is_ind_msg_ready());
  auto bytes = collector->get_indication_message();
  asn1::cbit_ref reader(bytes);
  asn1::e2sm::e2sm_kpm_ind_msg_s decoded;
  ASSERT_EQ(decoded.unpack(reader), asn1::OCUDUASN_SUCCESS);
  const auto& rows = decoded.ind_msg_formats.ind_msg_format3().ue_meas_report_list;
  ASSERT_EQ(rows.size(), 1U);
  EXPECT_EQ(rows[0].ue_id.gnb_ue_id().amf_ue_ngap_id, 0U);
  EXPECT_EQ(rows[0].ue_id.gnb_ue_id().gnb_cu_ue_f1ap_id_list[0].gnb_cu_ue_f1ap_id, 1007U);
  ASSERT_EQ(rows[0].meas_report.meas_data.size(), 2U);
  EXPECT_EQ(rows[0].meas_report.meas_data[1].meas_record[0].integer(), 1U);
}

TEST(cu_identity, duplicate_per_du_f1_ids_have_distinct_scoped_ran_identities)
{
  e2sm_kpm_cu_cp_meas_provider_impl provider;
  cu_cp_metrics_report report;
  for (unsigned i = 0; i < 2; ++i) {
    cu_cp_metrics_report::ue_info ue{};
    ue.ue_index = static_cast<ocucp::ue_index_t>(42 + i);
    ue.rrc_connection_state = ocucp::rrc_state::connected;
    guami_t guami{};
    guami.plmn = plmn_identity::parse("00101").value();
    guami.amf_region_id = 2;
    guami.amf_set_id = 1;
    guami.amf_pointer = 0;
    ue.identity = ocucp::cu_ue_identity{{(1ULL << 39) + i, 301 + i, guami}, {1, 1}, int_to_gnb_du_id(i + 1)};
    ue.slice_ssts = {1};
    report.ues.push_back(ue);
  }
  provider.report_metrics(report);
  std::vector<asn1::e2sm::ue_id_c> ids;
  asn1::e2sm::matching_ue_cond_per_sub_list_l conditions;
  ASSERT_TRUE(provider.get_ues_matching_test_conditions(conditions, ids));
  ASSERT_EQ(ids.size(), 2U);
  EXPECT_EQ(ids[0].gnb_ue_id().gnb_cu_ue_f1ap_id_list[0].gnb_cu_ue_f1ap_id, 1U);
  EXPECT_EQ(ids[1].gnb_ue_id().gnb_cu_ue_f1ap_id_list[0].gnb_cu_ue_f1ap_id, 1U);
  EXPECT_NE(ids[0].gnb_ue_id().ran_ue_id.to_number(), ids[1].gnb_ue_id().ran_ue_id.to_number());
  asn1::e2sm::e2sm_kpm_ind_msg_s message;
  auto& rows = message.ind_msg_formats.set_ind_msg_format3().ue_meas_report_list;
  rows.resize(2);
  for (unsigned i = 0; i < 2; ++i) {
    rows[i].ue_id = ids[i];
    auto& measurement = rows[i].meas_report;
    measurement.meas_info_list.resize(1);
    measurement.meas_info_list[0].meas_type.set_meas_name().from_string("FALCON.UE.RRCState");
    measurement.meas_info_list[0].label_info_list.resize(1);
    measurement.meas_info_list[0].label_info_list[0].meas_label.no_label_present = true;
    measurement.meas_info_list[0].label_info_list[0].meas_label.no_label =
        asn1::e2sm::meas_label_s::no_label_e_::true_value;
    measurement.meas_data.resize(1);
    asn1::e2sm::meas_record_item_c state;
    state.set_integer() = 1;
    measurement.meas_data[0].meas_record.push_back(state);
    measurement.granul_period_present = true;
    measurement.granul_period = 1000;
  }
  byte_buffer bytes;
  asn1::bit_ref writer(bytes);
  ASSERT_EQ(message.pack(writer), asn1::OCUDUASN_SUCCESS);
  asn1::e2sm::e2sm_kpm_ind_msg_s decoded;
  asn1::cbit_ref reader(bytes);
  ASSERT_EQ(decoded.unpack(reader), asn1::OCUDUASN_SUCCESS);
  const auto& result = decoded.ind_msg_formats.ind_msg_format3().ue_meas_report_list;
  EXPECT_EQ(result[0].ue_id.gnb_ue_id().ran_ue_id.to_number(), (uint64_t{1} << 28) | 42U);
  EXPECT_EQ(result[1].ue_id.gnb_ue_id().ran_ue_id.to_number(), (uint64_t{2} << 28) | 43U);
  if (const char* directory = std::getenv("FALCON_IDENTITY_FIXTURE_DIR")) {
    std::ofstream file(std::string(directory) + "/cu_scope_message.asn", std::ios::binary);
    for (auto byte : bytes) file.put(static_cast<char>(byte));
    ASSERT_TRUE(file.good());
  }
}

TEST(cu_identity, incomplete_context_is_never_published)
{
  e2sm_kpm_cu_cp_meas_provider_impl provider;
  cu_cp_metrics_report report;
  report.ues.emplace_back();
  report.ues[0].rrc_connection_state = ocucp::rrc_state::connected;
  provider.report_metrics(report);
  std::vector<asn1::e2sm::ue_id_c> ids;
  asn1::e2sm::matching_ue_cond_per_sub_list_l conditions;
  ASSERT_TRUE(provider.get_ues_matching_test_conditions(conditions, ids));
  EXPECT_TRUE(ids.empty());
}

TEST(cu_identity, scoped_ran_identity_preserves_bounds_without_truncation)
{
  e2sm_kpm_cu_cp_meas_provider_impl provider;
  cu_cp_metrics_report report;
  cu_cp_metrics_report::ue_info ue{};
  ue.ue_index = static_cast<ocucp::ue_index_t>((uint64_t{1} << 28) - 1);
  ue.rrc_connection_state = ocucp::rrc_state::connected;
  guami_t guami{};
  guami.plmn = plmn_identity::parse("00101").value();
  ue.identity = ocucp::cu_ue_identity{{1, 2, guami}, {1, 1}, gnb_du_id_t::max};
  report.ues.push_back(ue);
  provider.report_metrics(report);
  std::vector<asn1::e2sm::ue_id_c> ids;
  asn1::e2sm::matching_ue_cond_per_sub_list_l conditions;
  ASSERT_TRUE(provider.get_ues_matching_test_conditions(conditions, ids));
  ASSERT_EQ(ids.size(), 1U);
  EXPECT_EQ(ids[0].gnb_ue_id().ran_ue_id.to_number(), std::numeric_limits<uint64_t>::max());
  report.ues[0].ue_index = static_cast<ocucp::ue_index_t>(uint64_t{1} << 28);
  provider.report_metrics(report);
  ASSERT_TRUE(provider.get_ues_matching_test_conditions(conditions, ids));
  EXPECT_TRUE(ids.empty());
}

INSTANTIATE_TEST_SUITE_P(e2sm_kpm_du_meas_provider_test, e2_entity_test_with_pcap, testing::Values(g_pcap));
INSTANTIATE_TEST_SUITE_P(e2sm_kpm_du_meas_provider_test, e2sm_kpm_du_meas_provider_test, testing::Values(g_pcap));

int main(int argc, char** argv)
{
  // Check for '--enable_pcap' cmd line argument, do not use getopt as it interferes with gtest.
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--enable_pcap") {
      g_enable_pcap = true;
    }
  }

  ocudulog::init();

  std::unique_ptr<task_worker_executor> pcap_exec;
  std::unique_ptr<task_worker>          pcap_worker;
  std::unique_ptr<dlt_pcap>             common_pcap_writer;

  if (g_enable_pcap) {
    pcap_worker        = std::make_unique<task_worker>("pcap_worker", 128);
    pcap_exec          = std::make_unique<task_worker_executor>(*pcap_worker);
    common_pcap_writer = create_e2ap_pcap("/tmp/e2sm_kpm_meas_provider_test.pcap", *pcap_exec);
    g_pcap             = common_pcap_writer.get();
  }

  ::testing::InitGoogleTest(&argc, argv);
  int ret = RUN_ALL_TESTS();
  return ret;
}
