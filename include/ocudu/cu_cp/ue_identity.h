// SPDX-FileCopyrightText: Copyright (C) 2026 Ruibin Chen
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
#pragma once

#include "ocudu/cu_cp/cu_cp_types.h"
#include "ocudu/ran/gnb_du_id.h"
#include "ocudu/ran/guami.h"
#include <cstdint>

namespace ocudu::ocucp {

/// Owned identity views. Call getters only on the CU control executor.
struct ngap_ue_identity {
  uint64_t amf_ue_ngap_id;
  uint64_t ran_ue_ngap_id;
  guami_t  guami;
};

struct f1ap_ue_identity {
  uint64_t cu_ue_f1ap_id;
  uint64_t du_ue_f1ap_id;
};

struct cu_ue_identity {
  ngap_ue_identity ngap;
  f1ap_ue_identity f1;
  gnb_du_id_t      du_id;
};

} // namespace ocudu::ocucp
