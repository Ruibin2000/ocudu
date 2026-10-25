// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "e2sm_kpm_cu_meas_provider_impl.h"
#include <algorithm>

using namespace asn1::e2ap;
using namespace asn1::e2sm;
using namespace ocudu;

e2sm_kpm_cu_meas_provider_impl::e2sm_kpm_cu_meas_provider_impl() : logger(ocudulog::fetch_basic_logger("E2SM-KPM")) {}

e2sm_kpm_cu_cp_meas_provider_impl::e2sm_kpm_cu_cp_meas_provider_impl() : e2sm_kpm_cu_meas_provider_impl()
{
  // Vendor state gauge: actual CU RRC state, not an identity encoded as a metric.
  // 0=idle, 1=connected, 2=inactive. Identities travel in the standard UEID.
  supported_metrics.emplace("FALCON.UE.RRCState",
                            e2sm_kpm_supported_metric_t{
                                NO_LABEL, UE_LEVEL, false, &e2sm_kpm_cu_cp_meas_provider_impl::get_ue_rrc_state});
  supported_metrics.emplace(
      "RRC.ConnEstabAtt",
      e2sm_kpm_supported_metric_t{
          NO_LABEL, E2_NODE_LEVEL, false, &e2sm_kpm_cu_cp_meas_provider_impl::get_rrc_conn_estab_att});
  supported_metrics.emplace(
      "RRC.ConnEstabSucc",
      e2sm_kpm_supported_metric_t{
          NO_LABEL, E2_NODE_LEVEL, false, &e2sm_kpm_cu_cp_meas_provider_impl::get_rrc_conn_estab_succ});
  supported_metrics.emplace(
      "RRC.ConnEstabFailCause.NetworkReject",
      e2sm_kpm_supported_metric_t{NO_LABEL,
                                  E2_NODE_LEVEL,
                                  false,
                                  &e2sm_kpm_cu_cp_meas_provider_impl::get_rrc_conn_estab_fail_cause_network_reject});
  supported_metrics.emplace(
      "UECNTX.ConnEstabAtt",
      e2sm_kpm_supported_metric_t{
          NO_LABEL, E2_NODE_LEVEL, false, &e2sm_kpm_cu_cp_meas_provider_impl::get_uecntx_conn_estab_att});
  supported_metrics.emplace(
      "UECNTX.ConnEstabSucc",
      e2sm_kpm_supported_metric_t{
          NO_LABEL, E2_NODE_LEVEL, false, &e2sm_kpm_cu_cp_meas_provider_impl::get_uecntx_conn_estab_succ});
  supported_metrics.emplace(
      "RRC.ReEstabAtt",
      e2sm_kpm_supported_metric_t{
          NO_LABEL, E2_NODE_LEVEL, false, &e2sm_kpm_cu_cp_meas_provider_impl::get_rrc_reestab_att});
  supported_metrics.emplace(
      "RRC.ReEstabSuccWithUeContext",
      e2sm_kpm_supported_metric_t{
          NO_LABEL, E2_NODE_LEVEL, false, &e2sm_kpm_cu_cp_meas_provider_impl::get_rrc_reestab_succ_with_ue_context});
  supported_metrics.emplace("RRC.ConnMean",
                            e2sm_kpm_supported_metric_t{
                                NO_LABEL, E2_NODE_LEVEL, false, &e2sm_kpm_cu_cp_meas_provider_impl::get_rrc_conn_mean});
  supported_metrics.emplace("RRC.ConnMax",
                            e2sm_kpm_supported_metric_t{
                                NO_LABEL, E2_NODE_LEVEL, false, &e2sm_kpm_cu_cp_meas_provider_impl::get_rrc_conn_max});
}

namespace {

bool has_usable_identity(const cu_cp_metrics_report::ue_info& ue)
{
  return ue.identity && ue.identity->du_id != gnb_du_id_t::invalid &&
         ue.ue_index != ocucp::ue_index_t::invalid && static_cast<uint64_t>(ue.ue_index) < (uint64_t{1} << 28);
}

ue_id_c make_identity(const cu_cp_metrics_report::ue_info& ue)
{
  ue_id_c result;
  auto& id = result.set_gnb_ue_id();
  const auto& identity = *ue.identity;
  id.amf_ue_ngap_id = identity.ngap.amf_ue_ngap_id;
  id.guami.plmn_id = identity.ngap.guami.plmn.to_bytes();
  id.guami.amf_region_id.from_number(identity.ngap.guami.amf_region_id);
  id.guami.amf_set_id.from_number(identity.ngap.guami.amf_set_id);
  id.guami.amf_pointer.from_number(identity.ngap.guami.amf_pointer);
  id.gnb_cu_ue_f1ap_id_list.resize(1);
  id.gnb_cu_ue_f1ap_id_list[0].gnb_cu_ue_f1ap_id = identity.f1.cu_ue_f1ap_id;
  // Locally allocated RAN UE identity, using the existing optional 8-octet IE.
  // du36-cuue28-v1: full 36-bit serving DU ID followed by the CU UE index.
  // This is an opaque RAN UE ID on the wire, not an extra measurement. Only
  // consumers explicitly configured for this allocation may interpret it.
  // The CU index distinguishes simultaneously active contexts; it is NOT a
  // lifecycle generation counter. Reuse still requires conservative handling.
  id.ran_ue_id_present = true;
  id.ran_ue_id.from_number((gnb_du_id_to_int(identity.du_id) << 28) | static_cast<uint64_t>(ue.ue_index));
  return result;
}

bool matches_sst(const cu_cp_metrics_report::ue_info& ue, const test_cond_info_s& condition)
{
  if (condition.test_type.type() != test_cond_type_c::types::snssai || !condition.test_expr_present ||
      condition.test_expr != test_cond_expression_e::equal || !condition.test_value_present ||
      condition.test_value.type() != test_cond_value_c::types::value_int) {
    return false;
  }
  auto sst = condition.test_value.value_int();
  return sst >= 0 && sst <= 255 && std::find(ue.slice_ssts.begin(), ue.slice_ssts.end(), sst) != ue.slice_ssts.end();
}

} // namespace

bool e2sm_kpm_cu_cp_meas_provider_impl::is_ue_supported(const ue_id_c& id)
{
  std::lock_guard<std::mutex> lock(cu_cp_metrics_mutex);
  if (!identity_snapshot_is_fresh() || id.type() != ue_id_c::types::gnb_ue_id) {
    return false;
  }
  return std::any_of(cu_cp_metrics->ues.begin(), cu_cp_metrics->ues.end(), [&](const auto& ue) {
    return has_usable_identity(ue) && make_identity(ue) == id;
  });
}

bool e2sm_kpm_cu_cp_meas_provider_impl::is_test_cond_supported(const test_cond_type_c& type)
{
  return type.type() == test_cond_type_c::types::snssai;
}

bool e2sm_kpm_cu_cp_meas_provider_impl::get_ues_matching_test_conditions(
    const matching_ue_cond_per_sub_list_l& conditions, std::vector<ue_id_c>& ues)
{
  std::lock_guard<std::mutex> lock(cu_cp_metrics_mutex);
  ues.clear();
  if (!identity_snapshot_is_fresh()) {
    return true;
  }
  for (const auto& ue : cu_cp_metrics->ues) {
    if (!has_usable_identity(ue) || ue.rrc_connection_state != ocucp::rrc_state::connected) {
      continue;
    }
    bool matches = std::all_of(conditions.begin(), conditions.end(), [&](const auto& condition) {
      return !condition.lc_or_present && matches_sst(ue, condition.test_cond_info);
    });
    if (matches) {
      ues.push_back(make_identity(ue));
    }
  }
  return true;
}

bool e2sm_kpm_cu_cp_meas_provider_impl::get_ues_matching_test_conditions(
    const matching_cond_list_l& conditions, std::vector<ue_id_c>& ues)
{
  std::lock_guard<std::mutex> lock(cu_cp_metrics_mutex);
  ues.clear();
  if (!identity_snapshot_is_fresh()) {
    return true;
  }
  for (const auto& ue : cu_cp_metrics->ues) {
    if (!has_usable_identity(ue) || ue.rrc_connection_state != ocucp::rrc_state::connected) {
      continue;
    }
    bool matches = std::all_of(conditions.begin(), conditions.end(), [&](const auto& condition) {
      if (condition.lc_or_present) {
        return false;
      }
      const auto& choice = condition.matching_cond_choice;
      if (choice.type() == matching_cond_item_choice_c::types::test_cond_info) {
        return matches_sst(ue, choice.test_cond_info());
      }
      return choice.type() == matching_cond_item_choice_c::types::meas_label && choice.meas_label().no_label_present;
    });
    if (matches) {
      ues.push_back(make_identity(ue));
    }
  }
  return true;
}

bool e2sm_kpm_cu_meas_provider_impl::get_ue_rrc_state(const label_info_list_l labels,
                                                   const std::vector<ue_id_c>& ues,
                                                   const std::optional<cgi_c> cell,
                                                   std::vector<meas_record_item_c>& items)
{
  if (cell || ues.empty() || labels.size() != 1 || !labels[0].meas_label.no_label_present) {
    return false;
  }
  for (const auto& id : ues) {
    meas_record_item_c item;
    item.set_no_value();
    if (identity_snapshot_is_fresh()) {
      for (const auto& ue : cu_cp_metrics->ues) {
        if (has_usable_identity(ue) && make_identity(ue) == id) {
          item.set_integer() = static_cast<unsigned>(ue.rrc_connection_state);
          break;
        }
      }
    }
    items.push_back(item);
  }
  return true;
}

bool e2sm_kpm_cu_meas_provider_impl::check_e2sm_kpm_metrics_definitions(span<const e2sm_kpm_metric_t> metric_defs)
{
  std::string metric_name;
  auto        name_matches = [&metric_name](const e2sm_kpm_metric_t& x) {
    return (x.name == metric_name.c_str() or x.name == metric_name);
  };

  for (auto& supported_metric : supported_metrics) {
    metric_name = supported_metric.first;
    auto it     = std::find_if(metric_defs.begin(), metric_defs.end(), name_matches);
    if (it == metric_defs.end()) {
      continue;
    }

    if (supported_metric.second.supported_labels & ~(it->optional_labels | e2sm_kpm_label_enum::NO_LABEL)) {
      logger.debug("Wrong definition of the supported metric: \"{}\", labels cannot be supported.", metric_name);
    }

    if (supported_metric.second.supported_levels & ~it->optional_levels) {
      logger.debug("Wrong definition of the supported metric: \"{}\", level cannot be supported.", metric_name);
    }

    if (is_cell_id_required(*it) and not supported_metric.second.cell_scope_supported) {
      logger.debug("Wrong definition of the supported metric: \"{}\", cell scope has to be supported.",
                   metric_name.c_str());
    }
  }
  return true;
}

std::vector<std::string> e2sm_kpm_cu_meas_provider_impl::get_supported_metric_names(e2sm_kpm_metric_level_enum level)
{
  std::vector<std::string> metrics;
  for (auto& m : supported_metrics) {
    if ((level & E2_NODE_LEVEL) and (m.second.supported_levels & E2_NODE_LEVEL)) {
      metrics.push_back(m.first);
    } else if ((level & UE_LEVEL) and (m.second.supported_levels & UE_LEVEL)) {
      metrics.push_back(m.first);
    }
  }
  return metrics;
}

bool e2sm_kpm_cu_meas_provider_impl::is_cell_supported(const asn1::e2sm::cgi_c& cell_global_id)
{
  // TODO: check if CELL is supported
  return true;
}

bool e2sm_kpm_cu_meas_provider_impl::is_ue_supported(const asn1::e2sm::ue_id_c& ueid)
{
  // TODO: check if UE is supported
  return true;
}

bool e2sm_kpm_cu_meas_provider_impl::is_test_cond_supported(const asn1::e2sm::test_cond_type_c& test_cond_type)
{
  // TODO: check if test condition is supported
  return true;
}

bool e2sm_kpm_cu_meas_provider_impl::is_metric_supported(const asn1::e2sm::meas_type_c&   meas_type,
                                                         const asn1::e2sm::meas_label_s&  label,
                                                         const e2sm_kpm_metric_level_enum level,
                                                         const bool&                      cell_scope)
{
  if (meas_type.type() != meas_type_c::types::meas_name) {
    return false;
  }
  if (meas_type.meas_name().to_string() == "FALCON.UE.RRCState") {
    return supported_metrics.count("FALCON.UE.RRCState") != 0 && level == UE_LEVEL && !cell_scope && label.no_label_present;
  }
  if (!label.no_label_present) {
    logger.debug("Currently only NO_LABEL metric supported.");
    return false;
  }

  for (auto& metric : supported_metrics) {
    if (strcmp(meas_type.meas_name().to_string().c_str(), metric.first.c_str()) == 0) {
      return true;
    }
  }

  // TODO: check if metric supported with required label, level and cell_scope
  return false;
}

bool e2sm_kpm_cu_meas_provider_impl::get_ues_matching_test_conditions(
    const asn1::e2sm::matching_cond_list_l& matching_cond_list,
    std::vector<asn1::e2sm::ue_id_c>&       ues)
{
  return true;
}

bool e2sm_kpm_cu_meas_provider_impl::get_ues_matching_test_conditions(
    const asn1::e2sm::matching_ue_cond_per_sub_list_l& matching_ue_cond_list,
    std::vector<asn1::e2sm::ue_id_c>&                  ues)
{
  return true;
}

bool e2sm_kpm_cu_meas_provider_impl::get_meas_data(const asn1::e2sm::meas_type_c&               meas_type,
                                                   const asn1::e2sm::label_info_list_l          label_info_list,
                                                   const std::vector<asn1::e2sm::ue_id_c>&      ues,
                                                   const std::optional<asn1::e2sm::cgi_c>       cell_global_id,
                                                   std::vector<asn1::e2sm::meas_record_item_c>& items)
{
  std::lock_guard<std::mutex> lock(cu_cp_metrics_mutex);
  metric_meas_getter_func_ptr metric_meas_getter_func;
  auto                        it = supported_metrics.find(meas_type.meas_name().to_string().c_str());
  if (it == supported_metrics.end()) {
    logger.debug("Metric {} not supported.", meas_type.meas_name().to_string());
    return false;
  }
  metric_meas_getter_func = it->second.func;
  ocudu_assert(metric_meas_getter_func != nullptr, "Metric getter function cannot be empty.");
  return (this->*metric_meas_getter_func)(label_info_list, ues, cell_global_id, items);
}

bool e2sm_kpm_cu_meas_provider_impl::handle_no_meas_data_available(
    const std::vector<asn1::e2sm::ue_id_c>&        ues,
    std::vector<asn1::e2sm::meas_record_item_c>&   items,
    asn1::e2sm::meas_record_item_c::types::options value_type)
{
  if (ues.empty()) {
    // Fill with zero if E2 Node Measurement (Report Style 1)
    meas_record_item_c meas_record_item;
    if (value_type == asn1::e2sm::meas_record_item_c::types::options::integer) {
      meas_record_item.set_integer() = 0;
    } else if (value_type == asn1::e2sm::meas_record_item_c::types::options::real) {
      meas_record_item.set_real();
      meas_record_item.real().value = 0;
    } else if (value_type == asn1::e2sm::meas_record_item_c::types::options::not_satisfied) {
      meas_record_item.set_not_satisfied();
    } else {
      meas_record_item.set_no_value();
    }
    items.push_back(meas_record_item);
    return true;
  }
  return false;
}

float e2sm_kpm_cu_meas_provider_impl::bytes_to_kbits(float value)
{
  constexpr unsigned nof_bits_per_byte = 8;
  return (nof_bits_per_byte * value / 1e3);
}

void e2sm_kpm_cu_meas_provider_impl::report_metrics(const pdcp_metrics_container& metrics)
{
  ue_aggr_pdcp_metrics[metrics.ue_index].push_back(metrics);
  if (ue_aggr_pdcp_metrics[metrics.ue_index].size() > max_pdcp_metrics) {
    ue_aggr_pdcp_metrics[metrics.ue_index].pop_front();
  }
}

void e2sm_kpm_cu_meas_provider_impl::report_metrics(const ocuup::f1u_metrics_container& metrics)
{
  ue_aggr_f1u_metrics[metrics.ue_index].push_back(metrics);
  if (ue_aggr_f1u_metrics[metrics.ue_index].size() > max_f1u_metrics) {
    ue_aggr_f1u_metrics[metrics.ue_index].pop_front();
  }
}

void e2sm_kpm_cu_meas_provider_impl::report_metrics(const cu_cp_metrics_report& metrics)
{
  std::lock_guard<std::mutex> lock(cu_cp_metrics_mutex);
  cu_cp_metrics = metrics;
  cu_cp_metrics_updated = std::chrono::steady_clock::now();
}

e2sm_kpm_cu_up_meas_provider_impl::e2sm_kpm_cu_up_meas_provider_impl() : e2sm_kpm_cu_meas_provider_impl()
{
  supported_metrics.emplace(
      "DRB.PdcpReordDelayUl",
      e2sm_kpm_supported_metric_t{
          NO_LABEL, E2_NODE_LEVEL, false, &e2sm_kpm_cu_up_meas_provider_impl::get_pdcp_reordering_delay_ul});
  supported_metrics.emplace(
      "DRB.PacketSuccessRateUlgNBUu",
      e2sm_kpm_supported_metric_t{
          NO_LABEL, E2_NODE_LEVEL, false, &e2sm_kpm_cu_up_meas_provider_impl::get_packet_success_rate_ul_gnb_uu});
}

bool e2sm_kpm_cu_meas_provider_impl::get_pdcp_reordering_delay_ul(const asn1::e2sm::label_info_list_l label_info_list,
                                                                  const std::vector<asn1::e2sm::ue_id_c>& ues,
                                                                  const std::optional<asn1::e2sm::cgi_c> cell_global_id,
                                                                  std::vector<asn1::e2sm::meas_record_item_c>& items)
{
  bool meas_collected = false;
  if (ue_aggr_pdcp_metrics.empty()) {
    return handle_no_meas_data_available(ues, items, asn1::e2sm::meas_record_item_c::types::options::real);
  }
  if ((label_info_list.size() > 1 or
       (label_info_list.size() == 1 and not label_info_list[0].meas_label.no_label_present))) {
    logger.debug("Metric: DRB.PDCP supports only NO_LABEL label");
    return meas_collected;
  }
  if (ues.empty()) {
    // E2 level measurements.
    meas_record_item_c meas_record_item;
    float              av_ue_reordering_delay_us = 0;
    for (auto& pdcp_metric : ue_aggr_pdcp_metrics) {
      int num_reordering_records = std::accumulate(
          pdcp_metric.second.begin(),
          pdcp_metric.second.end(),
          0,
          [](size_t sum, const pdcp_metrics_container& metric) { return sum + metric.rx.reordering_counter; });
      int tot_reordering_delay_us = std::accumulate(
          pdcp_metric.second.begin(),
          pdcp_metric.second.end(),
          0,
          [](size_t sum, const pdcp_metrics_container& metric) { return sum + metric.rx.reordering_delay_us; });
      if (num_reordering_records && tot_reordering_delay_us) {
        av_ue_reordering_delay_us += (float)tot_reordering_delay_us / (float)num_reordering_records;
      }
    }
    if (av_ue_reordering_delay_us) {
      meas_record_item.set_real();
      meas_record_item.real().value = (av_ue_reordering_delay_us / ue_aggr_pdcp_metrics.size()) / 100; // unit is 0.1ms
      items.push_back(meas_record_item);
      meas_collected = true;
    } else {
      logger.warning("Invalid PDCP reordering delay value");
      return meas_collected;
    }
  }
  return meas_collected;
}

bool e2sm_kpm_cu_meas_provider_impl::get_packet_success_rate_ul_gnb_uu(const label_info_list_l          label_info_list,
                                                                       const std::vector<ue_id_c>&      ues,
                                                                       const std::optional<cgi_c>       cell_global_id,
                                                                       std::vector<meas_record_item_c>& items)
{
  bool meas_collected = false;
  if (ue_aggr_pdcp_metrics.empty()) {
    return handle_no_meas_data_available(ues, items, meas_record_item_c::types::options::no_value);
  }
  if ((label_info_list.size() > 1 or
       (label_info_list.size() == 1 and not label_info_list[0].meas_label.no_label_present))) {
    logger.debug("Metric: DRB.PacketSuccessRateUlgNBUu supports only NO_LABEL label");
    return meas_collected;
  }
  if (cell_global_id.has_value()) {
    logger.debug("Metric: DRB.PacketSuccessRateUlgNBUu currently does not support cell_global_id filter");
  }
  if (ues.empty()) {
    // E2 level measurements.
    meas_record_item_c meas_record_item;
    float              success_rate = 0;
    uint32_t           total_sdus   = 0;
    uint32_t           total_pdus   = 0;
    for (auto& ue_metric : ue_aggr_pdcp_metrics) {
      total_sdus += std::accumulate(
          ue_metric.second.begin(), ue_metric.second.end(), 0, [](size_t sum, const pdcp_metrics_container& metric) {
            return sum + metric.rx.num_sdus;
          });
      total_pdus += std::accumulate(
          ue_metric.second.begin(), ue_metric.second.end(), 0, [](size_t sum, const pdcp_metrics_container& metric) {
            return sum + metric.rx.num_data_pdus;
          });
    }
    if (total_pdus) {
      success_rate = 1.0 * (total_sdus / total_pdus);
    }
    uint32_t success_rate_int      = success_rate * 100;
    meas_record_item.set_integer() = success_rate_int;
    items.push_back(meas_record_item);
    meas_collected = true;
  }

  return meas_collected;
}

bool e2sm_kpm_cu_meas_provider_impl::get_rrc_conn_estab_att(const label_info_list_l          label_info_list,
                                                            const std::vector<ue_id_c>&      ues,
                                                            const std::optional<cgi_c>       cell_global_id,
                                                            std::vector<meas_record_item_c>& items)
{
  bool meas_collected = false;
  if (!cu_cp_metrics.has_value() || cu_cp_metrics->dus.empty()) {
    return handle_no_meas_data_available(ues, items, meas_record_item_c::types::options::integer);
  }
  if ((label_info_list.size() > 1 or
       (label_info_list.size() == 1 and not label_info_list[0].meas_label.no_label_present))) {
    logger.debug("Metric: RRC.ConnEstabAtt supports only NO_LABEL label");
    return meas_collected;
  }
  if (cell_global_id.has_value()) {
    logger.debug("Metric: RRC.ConnEstabAtt currently does not support cell_global_id filter");
  }
  if (ues.empty()) {
    meas_record_item_c meas_record_item;
    uint64_t           conn_estab_att = 0;
    for (const auto& du_metric : cu_cp_metrics->dus) {
      conn_estab_att += std::accumulate(du_metric.rrc_metrics.attempted_rrc_connection_establishments.begin(),
                                        du_metric.rrc_metrics.attempted_rrc_connection_establishments.end(),
                                        static_cast<uint64_t>(0));
    }
    meas_record_item.set_integer() = static_cast<int64_t>(conn_estab_att);
    items.push_back(meas_record_item);
    meas_collected = true;
  }
  return meas_collected;
}

bool e2sm_kpm_cu_meas_provider_impl::get_rrc_conn_estab_succ(const label_info_list_l          label_info_list,
                                                             const std::vector<ue_id_c>&      ues,
                                                             const std::optional<cgi_c>       cell_global_id,
                                                             std::vector<meas_record_item_c>& items)
{
  bool meas_collected = false;
  if (!cu_cp_metrics.has_value() || cu_cp_metrics->dus.empty()) {
    return handle_no_meas_data_available(ues, items, meas_record_item_c::types::options::integer);
  }
  if ((label_info_list.size() > 1 or
       (label_info_list.size() == 1 and not label_info_list[0].meas_label.no_label_present))) {
    logger.debug("Metric: RRC.ConnEstabSucc supports only NO_LABEL label");
    return meas_collected;
  }
  if (cell_global_id.has_value()) {
    logger.debug("Metric: RRC.ConnEstabSucc currently does not support cell_global_id filter");
  }
  if (ues.empty()) {
    meas_record_item_c meas_record_item;
    uint64_t           conn_estab_succ = 0;
    for (const auto& du_metric : cu_cp_metrics->dus) {
      conn_estab_succ += std::accumulate(du_metric.rrc_metrics.successful_rrc_connection_establishments.begin(),
                                         du_metric.rrc_metrics.successful_rrc_connection_establishments.end(),
                                         static_cast<uint64_t>(0));
    }
    meas_record_item.set_integer() = static_cast<int64_t>(conn_estab_succ);
    items.push_back(meas_record_item);
    meas_collected = true;
  }
  return meas_collected;
}

bool e2sm_kpm_cu_meas_provider_impl::get_rrc_conn_estab_fail_cause_network_reject(
    const label_info_list_l          label_info_list,
    const std::vector<ue_id_c>&      ues,
    const std::optional<cgi_c>       cell_global_id,
    std::vector<meas_record_item_c>& items)
{
  bool meas_collected = false;
  if (!cu_cp_metrics.has_value() || cu_cp_metrics->dus.empty()) {
    return handle_no_meas_data_available(ues, items, meas_record_item_c::types::options::integer);
  }
  if ((label_info_list.size() > 1 or
       (label_info_list.size() == 1 and not label_info_list[0].meas_label.no_label_present))) {
    logger.debug("Metric: RRC.ConnEstabFailCause.NetworkReject supports only NO_LABEL label");
    return meas_collected;
  }
  if (cell_global_id.has_value()) {
    logger.debug("Metric: RRC.ConnEstabFailCause.NetworkReject currently does not support cell_global_id filter");
  }
  if (ues.empty()) {
    meas_record_item_c meas_record_item;
    uint64_t           conn_estab_fail_network_reject = 0;
    for (const auto& du_metric : cu_cp_metrics->dus) {
      conn_estab_fail_network_reject += du_metric.rrc_metrics.failed_rrc_connection_establishments.get_count(
          establishment_fail_cause_t::network_reject);
    }
    meas_record_item.set_integer() = static_cast<int64_t>(conn_estab_fail_network_reject);
    items.push_back(meas_record_item);
    meas_collected = true;
  }
  return meas_collected;
}

bool e2sm_kpm_cu_meas_provider_impl::get_uecntx_conn_estab_att(const label_info_list_l          label_info_list,
                                                               const std::vector<ue_id_c>&      ues,
                                                               const std::optional<cgi_c>       cell_global_id,
                                                               std::vector<meas_record_item_c>& items)
{
  bool meas_collected = false;
  if (!cu_cp_metrics.has_value() || cu_cp_metrics->ngaps.empty()) {
    return handle_no_meas_data_available(ues, items, meas_record_item_c::types::options::integer);
  }
  if ((label_info_list.size() > 1 or
       (label_info_list.size() == 1 and not label_info_list[0].meas_label.no_label_present))) {
    logger.debug("Metric: UECNTX.ConnEstabAtt supports only NO_LABEL label");
    return meas_collected;
  }
  if (cell_global_id.has_value()) {
    logger.debug("Metric: UECNTX.ConnEstabAtt currently does not support cell_global_id filter");
  }
  if (ues.empty()) {
    meas_record_item_c meas_record_item;
    uint64_t           uecntx_conn_estab_att = 0;
    for (const auto& ngap_metric : cu_cp_metrics->ngaps) {
      uecntx_conn_estab_att += ngap_metric.metrics.nof_ue_associated_logical_ng_connection_establishment_attempts;
    }
    meas_record_item.set_integer() = static_cast<int64_t>(uecntx_conn_estab_att);
    items.push_back(meas_record_item);
    meas_collected = true;
  }
  return meas_collected;
}

bool e2sm_kpm_cu_meas_provider_impl::get_uecntx_conn_estab_succ(const label_info_list_l          label_info_list,
                                                                const std::vector<ue_id_c>&      ues,
                                                                const std::optional<cgi_c>       cell_global_id,
                                                                std::vector<meas_record_item_c>& items)
{
  bool meas_collected = false;
  if (!cu_cp_metrics.has_value() || cu_cp_metrics->ngaps.empty()) {
    return handle_no_meas_data_available(ues, items, meas_record_item_c::types::options::integer);
  }
  if ((label_info_list.size() > 1 or
       (label_info_list.size() == 1 and not label_info_list[0].meas_label.no_label_present))) {
    logger.debug("Metric: UECNTX.ConnEstabSucc supports only NO_LABEL label");
    return meas_collected;
  }
  if (cell_global_id.has_value()) {
    logger.debug("Metric: UECNTX.ConnEstabSucc currently does not support cell_global_id filter");
  }
  if (ues.empty()) {
    meas_record_item_c meas_record_item;
    uint64_t           uecntx_conn_estab_succ = 0;
    for (const auto& ngap_metric : cu_cp_metrics->ngaps) {
      uecntx_conn_estab_succ += ngap_metric.metrics.nof_ue_associated_logical_ng_connection_establishment_successes;
    }
    meas_record_item.set_integer() = static_cast<int64_t>(uecntx_conn_estab_succ);
    items.push_back(meas_record_item);
    meas_collected = true;
  }
  return meas_collected;
}

bool e2sm_kpm_cu_meas_provider_impl::get_rrc_reestab_att(const label_info_list_l          label_info_list,
                                                         const std::vector<ue_id_c>&      ues,
                                                         const std::optional<cgi_c>       cell_global_id,
                                                         std::vector<meas_record_item_c>& items)
{
  bool meas_collected = false;
  if (!cu_cp_metrics.has_value() || cu_cp_metrics->dus.empty()) {
    return handle_no_meas_data_available(ues, items, meas_record_item_c::types::options::integer);
  }
  if ((label_info_list.size() > 1 or
       (label_info_list.size() == 1 and not label_info_list[0].meas_label.no_label_present))) {
    logger.debug("Metric: RRC.ReEstabAtt supports only NO_LABEL label");
    return meas_collected;
  }
  if (cell_global_id.has_value()) {
    logger.debug("Metric: RRC.ReEstabAtt currently does not support cell_global_id filter");
  }
  if (ues.empty()) {
    meas_record_item_c meas_record_item;
    uint64_t           reestab_att = 0;
    for (const auto& du_metric : cu_cp_metrics->dus) {
      reestab_att += du_metric.rrc_metrics.attempted_rrc_connection_reestablishments;
    }
    meas_record_item.set_integer() = static_cast<int64_t>(reestab_att);
    items.push_back(meas_record_item);
    meas_collected = true;
  }
  return meas_collected;
}

bool e2sm_kpm_cu_meas_provider_impl::get_rrc_reestab_succ_with_ue_context(const label_info_list_l     label_info_list,
                                                                          const std::vector<ue_id_c>& ues,
                                                                          const std::optional<cgi_c>  cell_global_id,
                                                                          std::vector<meas_record_item_c>& items)
{
  bool meas_collected = false;
  if (!cu_cp_metrics.has_value() || cu_cp_metrics->dus.empty()) {
    return handle_no_meas_data_available(ues, items, meas_record_item_c::types::options::integer);
  }
  if ((label_info_list.size() > 1 or
       (label_info_list.size() == 1 and not label_info_list[0].meas_label.no_label_present))) {
    logger.debug("Metric: RRC.ReEstabSuccWithUeContext supports only NO_LABEL label");
    return meas_collected;
  }
  if (cell_global_id.has_value()) {
    logger.debug("Metric: RRC.ReEstabSuccWithUeContext currently does not support cell_global_id filter");
  }
  if (ues.empty()) {
    meas_record_item_c meas_record_item;
    uint64_t           reestab_succ_with_ue_context = 0;
    for (const auto& du_metric : cu_cp_metrics->dus) {
      reestab_succ_with_ue_context += du_metric.rrc_metrics.successful_rrc_connection_reestablishments_with_ue_context;
    }
    meas_record_item.set_integer() = static_cast<int64_t>(reestab_succ_with_ue_context);
    items.push_back(meas_record_item);
    meas_collected = true;
  }
  return meas_collected;
}

bool e2sm_kpm_cu_meas_provider_impl::get_rrc_conn_mean(const label_info_list_l          label_info_list,
                                                       const std::vector<ue_id_c>&      ues,
                                                       const std::optional<cgi_c>       cell_global_id,
                                                       std::vector<meas_record_item_c>& items)
{
  bool meas_collected = false;
  if (!cu_cp_metrics.has_value() || cu_cp_metrics->dus.empty()) {
    return handle_no_meas_data_available(ues, items, meas_record_item_c::types::options::integer);
  }
  if ((label_info_list.size() > 1 or
       (label_info_list.size() == 1 and not label_info_list[0].meas_label.no_label_present))) {
    logger.debug("Metric: RRC.ConnMean supports only NO_LABEL label");
    return meas_collected;
  }
  if (cell_global_id.has_value()) {
    logger.debug("Metric: RRC.ConnMean currently does not support cell_global_id filter");
  }
  if (ues.empty()) {
    meas_record_item_c meas_record_item;
    uint64_t           conn_mean = 0;
    for (const auto& du_metric : cu_cp_metrics->dus) {
      conn_mean += du_metric.rrc_metrics.mean_nof_rrc_connections;
    }
    meas_record_item.set_integer() = static_cast<int64_t>(conn_mean);
    items.push_back(meas_record_item);
    meas_collected = true;
  }
  return meas_collected;
}

bool e2sm_kpm_cu_meas_provider_impl::get_rrc_conn_max(const label_info_list_l          label_info_list,
                                                      const std::vector<ue_id_c>&      ues,
                                                      const std::optional<cgi_c>       cell_global_id,
                                                      std::vector<meas_record_item_c>& items)
{
  bool meas_collected = false;
  if (!cu_cp_metrics.has_value() || cu_cp_metrics->dus.empty()) {
    return handle_no_meas_data_available(ues, items, meas_record_item_c::types::options::integer);
  }
  if ((label_info_list.size() > 1 or
       (label_info_list.size() == 1 and not label_info_list[0].meas_label.no_label_present))) {
    logger.debug("Metric: RRC.ConnMax supports only NO_LABEL label");
    return meas_collected;
  }
  if (cell_global_id.has_value()) {
    logger.debug("Metric: RRC.ConnMax currently does not support cell_global_id filter");
  }
  if (ues.empty()) {
    meas_record_item_c meas_record_item;
    uint64_t           conn_max = 0;
    for (const auto& du_metric : cu_cp_metrics->dus) {
      conn_max = std::max<uint64_t>(conn_max, du_metric.rrc_metrics.max_nof_rrc_connections);
    }
    meas_record_item.set_integer() = static_cast<int64_t>(conn_max);
    items.push_back(meas_record_item);
    meas_collected = true;
  }
  return meas_collected;
}
