// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "lib/e2/e2sm/e2sm_kpm/e2sm_kpm_cu_meas_provider_impl.h"
#include "lib/e2/e2sm/e2sm_kpm/e2sm_kpm_du_meas_provider_impl.h"
#include "lib/e2/e2sm/e2sm_kpm/e2sm_kpm_asn1_packer.h"
#include <limits>
#include "tests/unittests/e2/common/e2_test_helpers.h"
#include "ocudu/ran/du_types.h"
#include <algorithm>
#include <gtest/gtest.h>
#include <map>

using namespace ocudu;
using namespace asn1::e2sm;

static span<const e2sm_kpm_metric_t> e2sm_kpm_28_552_metrics = get_e2sm_kpm_28_552_metrics();
static span<const e2sm_kpm_metric_t> e2sm_kpm_oran_metrics   = get_e2sm_kpm_oran_metrics();

bool get_metric_definition(std::string metric_name, e2sm_kpm_metric_t& e2sm_kpm_metric_def)
{
  auto name_matches = [&metric_name](const e2sm_kpm_metric_t& x) {
    return (x.name == metric_name.c_str() or x.name == metric_name);
  };

  const auto* it = std::find_if(e2sm_kpm_28_552_metrics.begin(), e2sm_kpm_28_552_metrics.end(), name_matches);
  if (it != e2sm_kpm_28_552_metrics.end()) {
    e2sm_kpm_metric_def = *it;
    return true;
  }

  it = std::find_if(e2sm_kpm_oran_metrics.begin(), e2sm_kpm_oran_metrics.end(), name_matches);
  if (it != e2sm_kpm_oran_metrics.end()) {
    e2sm_kpm_metric_def = *it;
    return true;
  }

  return false;
}

static rlc_metrics generate_non_zero_rlc_metrics(uint32_t ue_idx, uint32_t bearer_id)
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
  rlc_metric.rx.num_malformed_pdus = 1;
  rlc_metric.rx.sdu_latency_us     = 1000;

  rlc_metric.tx.tx_high.num_sdus                     = 10;
  rlc_metric.tx.tx_high.num_sdu_bytes                = rlc_metric.tx.tx_high.num_sdus * 1000;
  rlc_metric.tx.tx_high.num_dropped_sdus             = 1;
  rlc_metric.tx.tx_high.num_discarded_sdus           = 1;
  rlc_metric.tx.tx_high.num_discard_failures         = 1;
  rlc_metric.tx.tx_low.sum_sdu_latency_us            = 1000;
  rlc_metric.tx.tx_low.num_of_pulled_sdus            = 1;
  rlc_metric.tx.tx_low.num_pdus_no_segmentation      = 10;
  rlc_metric.tx.tx_low.num_pdu_bytes_no_segmentation = rlc_metric.tx.tx_low.num_pdus_no_segmentation * 1000;

  rlc_metric.tx.tx_low.mode_specific = rlc_am_tx_metrics_lower{};
  auto& am                           = std::get<rlc_am_tx_metrics_lower>(rlc_metric.tx.tx_low.mode_specific);
  am.num_pdus_with_segmentation      = 2;
  am.num_pdu_bytes_with_segmentation = am.num_pdus_with_segmentation * 1000;

  return rlc_metric;
}

static scheduler_cell_metrics generate_non_zero_sched_metrics()
{
  scheduler_cell_metrics sched_metric;
  sched_metric.nof_prbs            = 52;
  sched_metric.nof_dl_slots        = 14;
  sched_metric.nof_ul_slots        = 14;
  sched_metric.nof_prach_preambles = 10;

  scheduler_ue_metrics ue_metrics;
  ue_metrics.ue_index            = to_du_ue_index(0);
  ue_metrics.pci                 = 1;
  ue_metrics.rnti                = static_cast<rnti_t>(0x1000 + 1);
  ue_metrics.tot_pdsch_prbs_used = 1200;
  ue_metrics.tot_pusch_prbs_used = 1200;
  ue_metrics.avg_crc_delay_ms    = 100;
  ue_metrics.pusch_snr_db        = 10;
  for (auto i = 0; i < 10; i++) {
    ue_metrics.cqi_stats.update(i);
  }
  sched_metric.ue_metrics.push_back(ue_metrics);

  return sched_metric;
}

class dummy_e2_du_metrics_notifier : public e2_du_metrics_notifier, public e2_du_metrics_interface
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

class dummy_e2_cu_metrics_notifier : public e2_cu_metrics_notifier, public e2_cu_metrics_interface
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
  e2_cu_metrics_notifier* e2_meas_provider = nullptr;
};

class e2sm_kpm_meas_provider_metrics_test : public ::testing::Test
{
protected:
  void SetUp() override
  {
    ocudulog::fetch_basic_logger("TEST").set_level(ocudulog::basic_levels::debug);
    ocudulog::init();
    f1ap_ue_id_mapper = std::make_unique<dummy_f1ap_ue_id_translator>();
    du_meas_provider  = std::make_unique<e2sm_kpm_du_meas_provider_impl>(*f1ap_ue_id_mapper);
    metrics           = std::make_unique<dummy_e2_du_metrics_notifier>();
    metrics->connect_e2_du_meas_provider(du_meas_provider.get());
  }

  void TearDown() override
  {
    // Flush logger after each test.
    ocudulog::flush();
  }

  std::unique_ptr<dummy_e2_du_metrics_notifier>   metrics;
  std::unique_ptr<dummy_f1ap_ue_id_translator>    f1ap_ue_id_mapper;
  std::unique_ptr<e2sm_kpm_du_meas_provider_impl> du_meas_provider;
  ocudulog::basic_logger&                         test_logger = ocudulog::fetch_basic_logger("TEST");
};

TEST_F(e2sm_kpm_meas_provider_metrics_test, e2sm_kpm_supported_metrics_are_supported)
{
  e2sm_kpm_metric_level_enum metric_level;
  meas_type_c                meas_type;
  meas_label_s               meas_label;
  meas_label.no_label_present         = true;
  meas_label.no_label                 = meas_label_s::no_label_e_::true_value;
  bool                     cell_scope = false;
  std::vector<std::string> supported_metrics;
  bool                     metric_supported = false;

  // E2-NODE-LEVEL metrics
  metric_level      = E2_NODE_LEVEL;
  supported_metrics = du_meas_provider->get_supported_metric_names(metric_level);
  for (auto& metric : supported_metrics) {
    meas_type.set_meas_name().from_string(metric);
    metric_supported = du_meas_provider->is_metric_supported(meas_type, meas_label, metric_level, cell_scope);
    ASSERT_TRUE(metric_supported) << "Metric: " << metric << " Level: " << e2sm_kpm_scope_2_str(metric_level)
                                  << " returned as supported but not supported ";
  }

  // UE-LEVEL metrics
  metric_level      = UE_LEVEL;
  supported_metrics = du_meas_provider->get_supported_metric_names(metric_level);
  for (auto& metric : supported_metrics) {
    meas_type.set_meas_name().from_string(metric);
    metric_supported = du_meas_provider->is_metric_supported(meas_type, meas_label, metric_level, cell_scope);
    ASSERT_TRUE(metric_supported) << "Metric: " << metric << " Level: " << e2sm_kpm_scope_2_str(metric_level)
                                  << " returned as supported but not supported ";
  }
}

TEST_F(e2sm_kpm_meas_provider_metrics_test, e2sm_kpm_return_e2_level_metric_with_no_measurements)
{
  // metrics that return no_value when no measurements are present. Specifically, they should not return 0
  std::vector<std::string> no_value_metrics = {
      "DRB.AirIfDelayUl", "DRB.RlcSduDelayDl", "DRB.RlcDelayUl", "DRB.RlcPacketDropRateDl", "DRB.RlcSduDelayDl"};

  // E2-NODE-LEVEL metrics have to be always returned, even if 0 or NAN
  e2sm_kpm_metric_level_enum       metric_level = E2_NODE_LEVEL;
  meas_type_c                      meas_type;
  std::optional<asn1::e2sm::cgi_c> cell_global_id = {};
  e2sm_kpm_metric_t                e2sm_kpm_metric_definition;

  label_info_list_l label_info_list;
  label_info_item_s label_info_item           = {};
  label_info_item.meas_label.no_label_present = true;
  label_info_item.meas_label.no_label         = meas_label_s::no_label_e_::true_value;
  label_info_list.push_back(label_info_item);
  std::vector<meas_record_item_c> meas_records_items;

  std::vector<std::string> supported_metrics = du_meas_provider->get_supported_metric_names(metric_level);
  for (auto& metric : supported_metrics) {
    meas_type.set_meas_name().from_string(metric);

    meas_records_items.clear();
    du_meas_provider->get_meas_data(meas_type, label_info_list, {}, cell_global_id, meas_records_items);

    ASSERT_TRUE(meas_records_items.size() == 1)
        << "Metric: " << metric << " Level: " << e2sm_kpm_scope_2_str(metric_level)
        << " returned no measurements (size=" << meas_records_items.size() << ")";

    // Check if metric should return no_value when no measurements are present
    bool found = std::any_of(
        no_value_metrics.begin(), no_value_metrics.end(), [&metric](const std::string& s) { return s == metric; });
    if (found) {
      ASSERT_EQ(meas_records_items[0].type(), meas_record_item_c::types::no_value)
          << "Metric: " << metric << " Level: " << e2sm_kpm_scope_2_str(metric_level)
          << " expected to return no_value when no measurements available.";
      continue;
    }
    if (get_metric_definition(metric, e2sm_kpm_metric_definition)) {
      if (e2sm_kpm_metric_definition.data_type == e2sm_kpm_metric_dtype_t::INTEGER) {
        ASSERT_EQ(meas_records_items[0].type().value, meas_record_item_c::types::integer)
            << "Metric: " << e2sm_kpm_metric_definition.name.c_str() << " should return record of the integer type.";
      } else {
        // e2sm_kpm_metric_dtype_t::REAL
        ASSERT_EQ(meas_records_items[0].type().value, meas_record_item_c::types::real)
            << "Metric: " << e2sm_kpm_metric_definition.name.c_str() << " should return record of the real type.";
      }
    }

    switch (meas_records_items[0].type()) {
      case meas_record_item_c::types::integer:
        ASSERT_EQ(meas_records_items[0].integer(), 0)
            << "Metric: " << metric << " Level: " << e2sm_kpm_scope_2_str(metric_level);
        break;
      case meas_record_item_c::types::real:
        ASSERT_FLOAT_EQ(meas_records_items[0].real().value, 0.0)
            << "Metric: " << metric << " Level: " << e2sm_kpm_scope_2_str(metric_level);
        break;
      default:
        printf("%s type: %i\n", metric.c_str(), meas_records_items[0].type().value);
        ASSERT_TRUE(false) << "Metric: " << metric << " Level: " << e2sm_kpm_scope_2_str(metric_level)
                           << " returned a record with wrong type.";
        break;
    }
  }
}

TEST_F(e2sm_kpm_meas_provider_metrics_test, e2sm_kpm_return_e2_level_metric_with_with_measurements)
{
  // E2-NODE-LEVEL metrics have to be always returned, even if 0 or NAN
  e2sm_kpm_metric_level_enum       metric_level = E2_NODE_LEVEL;
  meas_type_c                      meas_type;
  std::optional<asn1::e2sm::cgi_c> cell_global_id = {};
  e2sm_kpm_metric_t                e2sm_kpm_metric_definition;

  label_info_list_l label_info_list;
  label_info_item_s label_info_item           = {};
  label_info_item.meas_label.no_label_present = true;
  label_info_item.meas_label.no_label         = meas_label_s::no_label_e_::true_value;
  label_info_list.push_back(label_info_item);
  std::vector<meas_record_item_c> meas_records_items;

  // Fill e2sm-kpm measurements provider with RLC and SCHED metrics.
  // Generate dummy metrics that will generate non-zero e2sm-kpm metric records.
  rlc_metrics rlc_metric = generate_non_zero_rlc_metrics(0, 1);
  metrics->report_metrics(rlc_metric);
  scheduler_cell_metrics sched_metrics = generate_non_zero_sched_metrics();
  metrics->report_metrics(sched_metrics);

  std::vector<std::string> supported_metrics = du_meas_provider->get_supported_metric_names(metric_level);
  for (auto& metric : supported_metrics) {
    meas_type.set_meas_name().from_string(metric);

    meas_records_items.clear();
    du_meas_provider->get_meas_data(meas_type, label_info_list, {}, cell_global_id, meas_records_items);

    ASSERT_TRUE(meas_records_items.size() == 1)
        << "Metric: " << metric << " Level: " << e2sm_kpm_scope_2_str(metric_level)
        << " returned no measurements (size=" << meas_records_items.size() << ")";

    if (get_metric_definition(metric, e2sm_kpm_metric_definition)) {
      if (e2sm_kpm_metric_definition.data_type == e2sm_kpm_metric_dtype_t::INTEGER) {
        ASSERT_EQ(meas_records_items[0].type().value, meas_record_item_c::types::integer)
            << "Metric: " << e2sm_kpm_metric_definition.name.c_str() << " should return record of the integer type.";
      } else {
        // e2sm_kpm_metric_dtype_t::REAL
        ASSERT_EQ(meas_records_items[0].type().value, meas_record_item_c::types::real)
            << "Metric: " << e2sm_kpm_metric_definition.name.c_str() << " should return record of the real type.";
      }
    }

    switch (meas_records_items[0].type()) {
      case meas_record_item_c::types::integer:
        ASSERT_NE(meas_records_items[0].integer(), 0)
            << "Metric: " << metric << " Level: " << e2sm_kpm_scope_2_str(metric_level);
        break;
      case meas_record_item_c::types::real:
        ASSERT_NE(meas_records_items[0].real().value, 0.0)
            << "Metric: " << metric << " Level: " << e2sm_kpm_scope_2_str(metric_level);
        break;
      default:
        printf("%s type: %i\n", metric.c_str(), meas_records_items[0].type().value);
        ASSERT_TRUE(false) << "Metric: " << metric << " Level: " << e2sm_kpm_scope_2_str(metric_level)
                           << " returned a record with wrong type.";
        break;
    }
  }
}

class e2sm_kpm_cu_cp_meas_provider_metrics_test : public ::testing::Test
{
protected:
  void SetUp() override
  {
    ocudulog::fetch_basic_logger("TEST").set_level(ocudulog::basic_levels::debug);
    ocudulog::init();
    cu_cp_meas_provider = std::make_unique<e2sm_kpm_cu_cp_meas_provider_impl>();
    metrics             = std::make_unique<dummy_e2_cu_metrics_notifier>();
    metrics->connect_e2_cu_meas_provider(cu_cp_meas_provider.get());
  }

  void TearDown() override
  {
    // Flush logger after each test.
    ocudulog::flush();
  }

  std::unique_ptr<dummy_e2_cu_metrics_notifier>      metrics;
  std::unique_ptr<e2sm_kpm_cu_cp_meas_provider_impl> cu_cp_meas_provider;
};

TEST_F(e2sm_kpm_cu_cp_meas_provider_metrics_test, e2sm_kpm_cu_cp_supported_metrics_are_present)
{
  std::vector<std::string> expected_metrics = {"RRC.ConnEstabAtt",
                                               "RRC.ConnEstabSucc",
                                               "RRC.ConnEstabFailCause.NetworkReject",
                                               "UECNTX.ConnEstabAtt",
                                               "UECNTX.ConnEstabSucc",
                                               "RRC.ReEstabAtt",
                                               "RRC.ReEstabSuccWithUeContext",
                                               "RRC.ConnMean",
                                               "RRC.ConnMax"};

  auto supported_metrics = cu_cp_meas_provider->get_supported_metric_names(E2_NODE_LEVEL);
  for (const auto& metric : expected_metrics) {
    auto it = std::find(supported_metrics.begin(), supported_metrics.end(), metric);
    ASSERT_TRUE(it != supported_metrics.end()) << "Missing supported metric: " << metric;
  }
}

TEST_F(e2sm_kpm_cu_cp_meas_provider_metrics_test, e2sm_kpm_cu_cp_returns_zero_without_metrics_report)
{
  label_info_list_l label_info_list;
  label_info_item_s label_info_item           = {};
  label_info_item.meas_label.no_label_present = true;
  label_info_item.meas_label.no_label         = meas_label_s::no_label_e_::true_value;
  label_info_list.push_back(label_info_item);

  meas_type_c                      meas_type;
  std::vector<meas_record_item_c>  meas_records_items;
  std::optional<asn1::e2sm::cgi_c> cell_global_id = {};

  auto supported_metrics = cu_cp_meas_provider->get_supported_metric_names(E2_NODE_LEVEL);
  for (const auto& metric : supported_metrics) {
    meas_type.set_meas_name().from_string(metric);
    meas_records_items.clear();

    ASSERT_TRUE(cu_cp_meas_provider->get_meas_data(meas_type, label_info_list, {}, cell_global_id, meas_records_items));
    ASSERT_EQ(meas_records_items.size(), 1);
    ASSERT_EQ(meas_records_items[0].type(), meas_record_item_c::types::integer);
    ASSERT_EQ(meas_records_items[0].integer(), 0);
  }
}

TEST_F(e2sm_kpm_cu_cp_meas_provider_metrics_test, e2sm_kpm_cu_cp_returns_expected_rrc_metrics)
{
  cu_cp_metrics_report report;
  report.dus.resize(2);

  report.dus[0].rrc_metrics.mean_nof_rrc_connections = 10;
  report.dus[0].rrc_metrics.max_nof_rrc_connections  = 12;
  report.dus[0].rrc_metrics.attempted_rrc_connection_establishments.increase(establishment_cause_t::emergency);
  report.dus[0].rrc_metrics.attempted_rrc_connection_establishments.increase(establishment_cause_t::high_prio_access);
  report.dus[0].rrc_metrics.successful_rrc_connection_establishments.increase(establishment_cause_t::emergency);
  report.dus[0].rrc_metrics.attempted_rrc_connection_reestablishments                  = 4;
  report.dus[0].rrc_metrics.successful_rrc_connection_reestablishments_with_ue_context = 3;

  report.dus[1].rrc_metrics.mean_nof_rrc_connections = 7;
  report.dus[1].rrc_metrics.max_nof_rrc_connections  = 20;
  report.dus[1].rrc_metrics.attempted_rrc_connection_establishments.increase(establishment_cause_t::mt_access);
  report.dus[1].rrc_metrics.successful_rrc_connection_establishments.increase(establishment_cause_t::mt_access);
  report.dus[1].rrc_metrics.successful_rrc_connection_establishments.increase(establishment_cause_t::mo_sig);
  for (unsigned i = 0; i != 5; ++i) {
    report.dus[1].rrc_metrics.failed_rrc_connection_establishments.increase(establishment_fail_cause_t::network_reject);
  }
  report.dus[1].rrc_metrics.attempted_rrc_connection_reestablishments                  = 6;
  report.dus[1].rrc_metrics.successful_rrc_connection_reestablishments_with_ue_context = 1;

  report.ngaps.resize(2);
  report.ngaps[0].metrics.nof_ue_associated_logical_ng_connection_establishment_attempts  = 4;
  report.ngaps[0].metrics.nof_ue_associated_logical_ng_connection_establishment_successes = 3;
  report.ngaps[1].metrics.nof_ue_associated_logical_ng_connection_establishment_attempts  = 2;
  report.ngaps[1].metrics.nof_ue_associated_logical_ng_connection_establishment_successes = 1;

  metrics->report_metrics(report);

  const std::map<std::string, int64_t> expected_values = {{"RRC.ConnEstabAtt", 3},
                                                          {"RRC.ConnEstabSucc", 3},
                                                          {"RRC.ConnEstabFailCause.NetworkReject", 5},
                                                          {"UECNTX.ConnEstabAtt", 6},
                                                          {"UECNTX.ConnEstabSucc", 4},
                                                          {"RRC.ReEstabAtt", 10},
                                                          {"RRC.ReEstabSuccWithUeContext", 4},
                                                          {"RRC.ConnMean", 17},
                                                          {"RRC.ConnMax", 20}};

  label_info_list_l label_info_list;
  label_info_item_s label_info_item           = {};
  label_info_item.meas_label.no_label_present = true;
  label_info_item.meas_label.no_label         = meas_label_s::no_label_e_::true_value;
  label_info_list.push_back(label_info_item);

  std::vector<meas_record_item_c>  meas_records_items;
  std::optional<asn1::e2sm::cgi_c> cell_global_id = {};
  meas_type_c                      meas_type;

  for (const auto& metric : expected_values) {
    meas_type.set_meas_name().from_string(metric.first);
    meas_records_items.clear();

    ASSERT_TRUE(cu_cp_meas_provider->get_meas_data(meas_type, label_info_list, {}, cell_global_id, meas_records_items));
    ASSERT_EQ(meas_records_items.size(), 1);
    ASSERT_EQ(meas_records_items[0].type(), meas_record_item_c::types::integer);
    ASSERT_EQ(meas_records_items[0].integer(), metric.second) << "Unexpected value for metric " << metric.first;
  }
}

namespace {
constexpr const char* falcon_rsrp_name = "FALCON.PUSCH.RSRP.dBFS";
constexpr const char* falcon_snr_name = "FALCON.PUSCH.SNR.dB";

class falcon_sparse_ue_id_translator : public odu::f1ap_ue_id_translator
{
public:
  void remove_ue(du_ue_index_t index) { ids.erase(index); }

  std::optional<gnb_cu_ue_f1ap_id_t> get_gnb_cu_ue_f1ap_id(const du_ue_index_t& index) const override
  {
    const auto it = ids.find(index);
    if (it == ids.end()) {
      return std::nullopt;
    }
    return int_to_gnb_cu_ue_f1ap_id(it->second);
  }

  std::optional<gnb_cu_ue_f1ap_id_t> get_gnb_cu_ue_f1ap_id(const gnb_du_ue_f1ap_id_t& id) const override
  {
    return get_gnb_cu_ue_f1ap_id(to_du_ue_index(gnb_du_ue_f1ap_id_to_uint(id)));
  }

  gnb_du_ue_f1ap_id_t get_gnb_du_ue_f1ap_id(const du_ue_index_t& index) override
  {
    return int_to_gnb_du_ue_f1ap_id(index);
  }

  gnb_du_ue_f1ap_id_t get_gnb_du_ue_f1ap_id(const gnb_cu_ue_f1ap_id_t& id) override
  {
    return int_to_gnb_du_ue_f1ap_id(get_ue_index(id));
  }

  du_ue_index_t get_ue_index(const gnb_du_ue_f1ap_id_t& id) override
  {
    const auto index = to_du_ue_index(gnb_du_ue_f1ap_id_to_uint(id));
    return ids.count(index) == 0 ? INVALID_DU_UE_INDEX : index;
  }

  du_ue_index_t get_ue_index(const gnb_cu_ue_f1ap_id_t& id) override
  {
    for (const auto& pair : ids) {
      if (pair.second == gnb_cu_ue_f1ap_id_to_uint(id)) {
        return pair.first;
      }
    }
    return INVALID_DU_UE_INDEX;
  }

private:
  std::map<du_ue_index_t, uint32_t> ids{
      {to_du_ue_index(7), 1007}, {to_du_ue_index(91), 2091}, {to_du_ue_index(199), 3199}};
};

ue_id_c make_ue(uint32_t f1ap_id)
{
  ue_id_c ue;
  ue.set_gnb_du_ue_id().gnb_cu_ue_f1ap_id = f1ap_id;
  return ue;
}

label_info_list_l make_no_label()
{
  label_info_item_s item;
  item.meas_label.no_label_present = true;
  item.meas_label.no_label         = meas_label_s::no_label_e_::true_value;
  label_info_list_l result;
  result.push_back(item);
  return result;
}

scheduler_ue_metrics make_measurement(unsigned index, float power, float snr = 23.75F, unsigned samples = 1)
{
  scheduler_ue_metrics result{};
  result.ue_index      = to_du_ue_index(index);
  result.pci           = 1;
  result.rnti          = static_cast<rnti_t>(0x4600 + index);
  result.pusch_rsrp_db = power;
  result.pusch_snr_db  = snr; // The PHY-selected ul_sinr_dB value is reported using the SNR name.
  result.nof_pusch_snr_reports = samples;
  return result;
}

class falcon_du_rf_native_test : public ::testing::Test
{
protected:
  falcon_du_rf_native_test() : provider(translator)
  {
    metric.set_meas_name().from_string(falcon_rsrp_name);
  }

  static void SetUpTestSuite() { ocudulog::init(); }
  void TearDown() override { ocudulog::flush(); }

  void report(std::initializer_list<scheduler_ue_metrics> measurements)
  {
    scheduler_cell_metrics snapshot{};
    for (const auto& measurement : measurements) {
      snapshot.ue_metrics.push_back(measurement);
    }
    provider.report_metrics(snapshot);
  }

  bool collect(const std::vector<ue_id_c>& ues, std::vector<meas_record_item_c>& output)
  {
    return provider.get_meas_data(metric, make_no_label(), ues, std::nullopt, output);
  }

  bool collect_named(const char* name, const std::vector<ue_id_c>& ues, std::vector<meas_record_item_c>& output)
  {
    meas_type_c selected;
    selected.set_meas_name().from_string(name);
    return provider.get_meas_data(selected, make_no_label(), ues, std::nullopt, output);
  }

  falcon_sparse_ue_id_translator              translator;
  e2sm_kpm_du_meas_provider_impl       provider;
  meas_type_c                         metric;
};


TEST_F(falcon_du_rf_native_test, sparse_f1ap_mapping_preserves_request_order_and_real_values)
{
  report({make_measurement(91, 0.F, 0.F), make_measurement(7, -13.125F, 21.75F)});
  for (const char* name : {falcon_rsrp_name, falcon_snr_name}) {
    SCOPED_TRACE(name);
    std::vector<meas_record_item_c> output;
    ASSERT_TRUE(collect_named(name, {make_ue(1007), make_ue(3199), make_ue(2091), make_ue(9999)}, output));
    ASSERT_EQ(output.size(), 4U);
    ASSERT_EQ(output[0].type(), meas_record_item_c::types::real);
    EXPECT_DOUBLE_EQ(output[0].real().value, name == falcon_rsrp_name ? -13.125 : 21.75);
    EXPECT_EQ(output[1].type(), meas_record_item_c::types::no_value);
    ASSERT_EQ(output[2].type(), meas_record_item_c::types::real);
    EXPECT_DOUBLE_EQ(output[2].real().value, 0.0);
    EXPECT_EQ(output[3].type(), meas_record_item_c::types::no_value);
  }
}

TEST_F(falcon_du_rf_native_test, nonfinite_values_and_zero_snr_without_samples_are_no_value)
{
  for (float invalid : {std::numeric_limits<float>::quiet_NaN(),
                        std::numeric_limits<float>::infinity(),
                        -std::numeric_limits<float>::infinity()}) {
    report({make_measurement(7, invalid, invalid)});
    for (const char* name : {falcon_rsrp_name, falcon_snr_name}) {
      std::vector<meas_record_item_c> output;
      ASSERT_TRUE(collect_named(name, {make_ue(1007)}, output));
      ASSERT_EQ(output.size(), 1U);
      EXPECT_EQ(output[0].type(), meas_record_item_c::types::no_value);
    }
  }
  report({make_measurement(7, 0.F, 0.F, 0)});
  std::vector<meas_record_item_c> output;
  ASSERT_TRUE(collect_named(falcon_snr_name, {make_ue(1007)}, output));
  ASSERT_EQ(output.size(), 1U);
  EXPECT_EQ(output[0].type(), meas_record_item_c::types::no_value);
  output.clear();
  ASSERT_TRUE(collect_named(falcon_rsrp_name, {make_ue(1007)}, output));
  ASSERT_EQ(output.size(), 1U);
  ASSERT_EQ(output[0].type(), meas_record_item_c::types::real);
  EXPECT_DOUBLE_EQ(output[0].real().value, 0.0);
}

TEST_F(falcon_du_rf_native_test, replacement_snapshot_and_unsupported_ue_do_not_reuse_first_ue)
{
  report({make_measurement(7, -13.125F, 21.75F)});
  report({make_measurement(91, -3.5F, 7.25F)});
  ue_id_c wrong_type;
  wrong_type.set_gnb_ue_id();
  for (const char* name : {falcon_rsrp_name, falcon_snr_name}) {
    std::vector<meas_record_item_c> output;
    ASSERT_TRUE(collect_named(name, {make_ue(1007), wrong_type, make_ue(2091)}, output));
    ASSERT_EQ(output.size(), 3U);
    EXPECT_EQ(output[0].type(), meas_record_item_c::types::no_value);
    EXPECT_EQ(output[1].type(), meas_record_item_c::types::no_value);
    ASSERT_EQ(output[2].type(), meas_record_item_c::types::real);
    EXPECT_DOUBLE_EQ(output[2].real().value, name == falcon_rsrp_name ? -3.5 : 7.25);
  }
}

TEST_F(falcon_du_rf_native_test, exactly_one_no_label_and_ue_level_are_required)
{
  auto mixed = make_no_label();
  mixed[0].meas_label.five_qi_present = true;
  mixed[0].meas_label.five_qi = 9;
  auto duplicate = make_no_label();
  duplicate.push_back(duplicate[0]);
  for (const char* name : {falcon_rsrp_name, falcon_snr_name}) {
    meas_type_c selected;
    selected.set_meas_name().from_string(name);
    EXPECT_TRUE(provider.is_metric_supported(selected, make_no_label()[0].meas_label, UE_LEVEL, false));
    EXPECT_FALSE(provider.is_metric_supported(selected, make_no_label()[0].meas_label, E2_NODE_LEVEL, false));
    EXPECT_FALSE(provider.is_metric_supported(selected, make_no_label()[0].meas_label, UE_LEVEL, true));
    EXPECT_FALSE(provider.is_metric_supported(selected, mixed[0].meas_label, UE_LEVEL, false));
    for (const auto& labels : {label_info_list_l{}, mixed, duplicate}) {
      std::vector<meas_record_item_c> output;
      EXPECT_FALSE(provider.get_meas_data(selected, labels, {make_ue(1007), make_ue(2091)}, std::nullopt, output));
      ASSERT_EQ(output.size(), 2U);
      EXPECT_EQ(output[0].type(), meas_record_item_c::types::no_value);
      EXPECT_EQ(output[1].type(), meas_record_item_c::types::no_value);
    }
  }
}

TEST_F(falcon_du_rf_native_test, production_ran_function_advertises_rf_names_without_dummy_rsrp_rsrq)
{
  e2sm_kpm_asn1_packer packer(provider);
  auto encoded = packer.pack_ran_function_description();
  ASSERT_FALSE(encoded.empty());
  asn1::cbit_ref reader(encoded);
  e2sm_kpm_ran_function_description_s decoded;
  ASSERT_EQ(decoded.unpack(reader), asn1::OCUDUASN_SUCCESS);
  unsigned ue_styles = 0;
  for (const auto& style : decoded.ric_report_style_list) {
    unsigned rsrp_count = 0;
    unsigned snr_count = 0;
    for (const auto& info : style.meas_info_action_list) {
      const auto name = info.meas_name.to_string();
      rsrp_count += name == falcon_rsrp_name;
      snr_count += name == falcon_snr_name;
      EXPECT_NE(name, "RSRP");
      EXPECT_NE(name, "RSRQ");
      EXPECT_NE(name, "FALCON.PUSCH.SINR.dB");
    }
    const bool ue_level = style.ric_report_style_type >= 2 && style.ric_report_style_type <= 5;
    EXPECT_EQ(rsrp_count, ue_level ? 1U : 0U);
    EXPECT_EQ(snr_count, ue_level ? 1U : 0U);
    ue_styles += ue_level;
    if (style.ric_report_style_type == 4) {
      EXPECT_EQ(style.ric_action_format_type, 4);
      EXPECT_EQ(style.ric_ind_msg_format_type, 3);
    }
  }
  EXPECT_EQ(ue_styles, 4U);
}
} // namespace
