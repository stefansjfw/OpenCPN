/***************************************************************************
 *
 * Project:  OpenCPN
 * Purpose:
 * Author:   David Register, Alec Leamas
 *
 ***************************************************************************
 *   Copyright (C) 2022 by David Register, Alec Leamas                     *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301,  USA.         *
 **************************************************************************/

#include "wx/wxprec.h"

#ifndef WX_PRECOMP
#include "wx/wx.h"
#endif  // precompiled headers
#include "wx/tokenzr.h"

#include "comm_bridge.h"
#include "comm_appmsg_bus.h"
#include "comm_drv_registry.h"
#include "idents.h"
#include "ocpn_types.h"
#include "comm_ais.h"

//  comm event definitions
wxDEFINE_EVENT(EVT_N2K_129029, ObservedEvt);
wxDEFINE_EVENT(EVT_N2K_129025, ObservedEvt);
wxDEFINE_EVENT(EVT_N2K_129026, ObservedEvt);
wxDEFINE_EVENT(EVT_N2K_127250, ObservedEvt);
wxDEFINE_EVENT(EVT_N2K_129540, ObservedEvt);

wxDEFINE_EVENT(EVT_N0183_RMC, ObservedEvt);
wxDEFINE_EVENT(EVT_N0183_HDT, ObservedEvt);
wxDEFINE_EVENT(EVT_N0183_HDG, ObservedEvt);
wxDEFINE_EVENT(EVT_N0183_HDM, ObservedEvt);
wxDEFINE_EVENT(EVT_N0183_VTG, ObservedEvt);
wxDEFINE_EVENT(EVT_N0183_GSV, ObservedEvt);
wxDEFINE_EVENT(EVT_N0183_GGA, ObservedEvt);
wxDEFINE_EVENT(EVT_N0183_GLL, ObservedEvt);
wxDEFINE_EVENT(EVT_N0183_AIVDO, ObservedEvt);

wxDEFINE_EVENT(EVT_DRIVER_CHANGE, wxCommandEvent);

wxDEFINE_EVENT(EVT_SIGNALK, ObservedEvt);

extern double gLat, gLon, gCog, gSog, gHdt, gHdm, gVar;
extern wxString gRmcDate, gRmcTime;
extern int g_nNMEADebug;
extern int g_priSats, g_SatsInView;
extern bool g_bSatValid;
extern bool g_bHDT_Rx, g_bVAR_Rx;
extern double g_UserVar;
extern int gps_watchdog_timeout_ticks;
extern int sat_watchdog_timeout_ticks;
extern wxString g_ownshipMMSI_SK;

bool debug_priority = 1;

void ClearNavData(NavData &d){
  d.gLat = NAN;
  d.gLon = NAN;
  d.gSog = NAN;
  d.gCog = NAN;
  d.gHdt = NAN;
  d.gHdm = NAN;
  d.gVar = NAN;
  d.n_satellites = -1;
  d.SID = 0;
}

static inline double GeodesicRadToDeg(double rads) {
  return rads * 180.0 / M_PI;
}

static inline double MS2KNOTS(double ms) {
  return ms * 1.9438444924406;
}

// CommBridge implementation

BEGIN_EVENT_TABLE(CommBridge, wxEvtHandler)
EVT_TIMER(WATCHDOG_TIMER, CommBridge::OnWatchdogTimer)
END_EVENT_TABLE()

CommBridge::CommBridge() {}

CommBridge::~CommBridge() {}

bool CommBridge::Initialize() {

  InitializePriorityContainers();

  // Clear the watchdogs
  PresetWatchdogs();

  m_watchdog_timer.SetOwner(this, WATCHDOG_TIMER);
  m_watchdog_timer.Start(1000, wxTIMER_CONTINUOUS);

  // Initialize the comm listeners
  InitCommListeners();

  // Initialize a listener for driver state changes
  driver_change_listener = CommDriverRegistry::getInstance()
            .evt_driverlist_change.GetListener(this, EVT_DRIVER_CHANGE);
  Bind(EVT_DRIVER_CHANGE, [&](wxCommandEvent ev) {
       OnDriverStateChange(); });

  return true;
}

void CommBridge::PresetWatchdogs() {
  m_watchdogs.position_watchdog = 5;
  m_watchdogs.velocity_watchdog = 5;
  m_watchdogs.variation_watchdog = 5;
  m_watchdogs.heading_watchdog = 5;
  m_watchdogs.satellite_watchdog = 5;
}

void CommBridge::OnWatchdogTimer(wxTimerEvent& event) {
  //  Update and check watchdog timer for GPS data source
  m_watchdogs.position_watchdog--;
  if (m_watchdogs.position_watchdog <= 0) {
    if (m_watchdogs.position_watchdog % 5 == 0) {
      // Send AppMsg telling of watchdog expiry
      auto msg = std::make_shared<GPSWatchdogMsg>(m_watchdogs.position_watchdog);
      auto& msgbus = AppMsgBus::GetInstance();
      msgbus.Notify(std::move(msg));

      wxString logmsg;
      logmsg.Printf(_T("   ***GPS Watchdog timeout at Lat:%g   Lon: %g"), gLat,
                    gLon);
      wxLogMessage(logmsg);
    }

    gSog = NAN;
    gCog = NAN;
    gRmcDate.Empty();
    gRmcTime.Empty();

    // Are there any other lower priority sources?
    // If so, adopt that one.
    for (auto it = priority_map_position.begin(); it != priority_map_position.end(); it++) {
      if (it->second > active_priority_position.active_priority){
          active_priority_position.active_priority = it->second;
          break;
      }
    }

    active_priority_position.active_source.clear();
    active_priority_position.active_identifier.clear();
  }

  //FIXME (dave) Do we need to bump the current priority for these watchdogs?

  //  Update and check watchdog timer for True Heading data source
  m_watchdogs.heading_watchdog--;
  if (m_watchdogs.heading_watchdog <= 0) {
    g_bHDT_Rx = false;
    gHdt = NAN;
    if (g_nNMEADebug && (m_watchdogs.heading_watchdog == 0))
      wxLogMessage(_T("   ***HDT Watchdog timeout..."));
  }

  //  Update and check watchdog timer for Magnetic Variation data source
  m_watchdogs.variation_watchdog--;
  if (m_watchdogs.variation_watchdog <= 0) {
    g_bVAR_Rx = false;
    if (g_nNMEADebug && (m_watchdogs.variation_watchdog == 0))
      wxLogMessage(_T("   ***VAR Watchdog timeout..."));
  }

  //  Update and check watchdog timer for GSV, GGA and SignalK (Satellite data)
  m_watchdogs.satellite_watchdog--;
  if (m_watchdogs.satellite_watchdog <= 0) {
    g_bSatValid = false;
    g_SatsInView = 0;
    g_priSats = 99;
    if (g_nNMEADebug && (m_watchdogs.satellite_watchdog == 0))
      wxLogMessage(_T("   ***SAT Watchdog timeout..."));
  }
}

void CommBridge::MakeHDTFromHDM() {
  //    Here is the one place we try to create gHdt from gHdm and gVar,
  //    but only if NMEA HDT sentence is not being received

  if (!g_bHDT_Rx) {
    if (!std::isnan(gHdm)) {
      // Set gVar if needed from manual entry. gVar will be overwritten if
      // WMM plugin is available
      if (std::isnan(gVar) && (g_UserVar != 0.0)) gVar = g_UserVar;
      gHdt = gHdm + gVar;
      if (!std::isnan(gHdt)) {
        if (gHdt < 0)
          gHdt += 360.0;
        else if (gHdt >= 360)
          gHdt -= 360.0;

        m_watchdogs.heading_watchdog = gps_watchdog_timeout_ticks;
      }

    }
  }
}

void CommBridge::InitCommListeners() {
  // Initialize the comm listeners
  auto& msgbus = NavMsgBus::GetInstance();

  // GNSS Position Data PGN  129029
  //----------------------------------
  Nmea2000Msg n2k_msg_129029(static_cast<uint64_t>(129029));

  listener_N2K_129029 =
      msgbus.GetListener(EVT_N2K_129029, this, n2k_msg_129029);

  Bind(EVT_N2K_129029, [&](ObservedEvt ev) {
    HandleN2K_129029(UnpackEvtPointer<Nmea2000Msg>(ev));
  });

  // Position rapid   PGN 129025
  //-----------------------------
  Nmea2000Msg n2k_msg_129025(static_cast<uint64_t>(129025));
  listener_N2K_129025 =
      msgbus.GetListener(EVT_N2K_129025, this, n2k_msg_129025);
  Bind(EVT_N2K_129025, [&](ObservedEvt ev) {
    HandleN2K_129025(UnpackEvtPointer<Nmea2000Msg>(ev));
  });

  // COG SOG rapid   PGN 129026
  //-----------------------------
  Nmea2000Msg n2k_msg_129026(static_cast<uint64_t>(129026));
  listener_N2K_129026 =
      msgbus.GetListener(EVT_N2K_129026, this, n2k_msg_129026);
  Bind(EVT_N2K_129026, [&](ObservedEvt ev) {
    HandleN2K_129026(UnpackEvtPointer<Nmea2000Msg>(ev));
  });

  // Heading rapid   PGN 127250
  //-----------------------------
  Nmea2000Msg n2k_msg_127250(static_cast<uint64_t>(127250));
  listener_N2K_127250 =
      msgbus.GetListener(EVT_N2K_127250, this, n2k_msg_127250);
  Bind(EVT_N2K_127250, [&](ObservedEvt ev) {
    HandleN2K_127250(UnpackEvtPointer<Nmea2000Msg>(ev));
  });

  // GNSS Satellites in View   PGN 129540
  //-----------------------------
  Nmea2000Msg n2k_msg_129540(static_cast<uint64_t>(129540));
  listener_N2K_129540 =
      msgbus.GetListener(EVT_N2K_129540, this, n2k_msg_129540);
  Bind(EVT_N2K_129540, [&](ObservedEvt ev) {
    HandleN2K_129540(UnpackEvtPointer<Nmea2000Msg>(ev));
  });


  // NMEA0183
  // RMC
  Nmea0183Msg n0183_msg_RMC("RMC");
  listener_N0183_RMC = msgbus.GetListener(EVT_N0183_RMC, this, n0183_msg_RMC);

  Bind(EVT_N0183_RMC, [&](ObservedEvt ev) {
    HandleN0183_RMC(UnpackEvtPointer<Nmea0183Msg>(ev));
  });

  // HDT
  Nmea0183Msg n0183_msg_HDT("HDT");
  listener_N0183_HDT = msgbus.GetListener(EVT_N0183_HDT, this, n0183_msg_HDT);

  Bind(EVT_N0183_HDT, [&](ObservedEvt ev) {
    HandleN0183_HDT(UnpackEvtPointer<Nmea0183Msg>(ev));
  });

  // HDG
  Nmea0183Msg n0183_msg_HDG("HDG");
  listener_N0183_HDG = msgbus.GetListener(EVT_N0183_HDG, this, n0183_msg_HDG);

  Bind(EVT_N0183_HDG, [&](ObservedEvt ev) {
    HandleN0183_HDG(UnpackEvtPointer<Nmea0183Msg>(ev));
  });

  // HDM
  Nmea0183Msg n0183_msg_HDM("HDM");
  listener_N0183_HDM = msgbus.GetListener(EVT_N0183_HDM, this, n0183_msg_HDM);

  Bind(EVT_N0183_HDM, [&](ObservedEvt ev) {
    HandleN0183_HDM(UnpackEvtPointer<Nmea0183Msg>(ev));
  });

  // VTG
  Nmea0183Msg n0183_msg_VTG("VTG");
  listener_N0183_VTG = msgbus.GetListener(EVT_N0183_VTG, this, n0183_msg_VTG);

  Bind(EVT_N0183_VTG, [&](ObservedEvt ev) {
    HandleN0183_VTG(UnpackEvtPointer<Nmea0183Msg>(ev));
  });

  // GSV
  Nmea0183Msg n0183_msg_GSV("GSV");
  listener_N0183_GSV = msgbus.GetListener(EVT_N0183_GSV, this, n0183_msg_GSV);

  Bind(EVT_N0183_GSV, [&](ObservedEvt ev) {
    HandleN0183_GSV(UnpackEvtPointer<Nmea0183Msg>(ev));
  });

  // GGA
  Nmea0183Msg n0183_msg_GGA("GGA");
  listener_N0183_GGA = msgbus.GetListener(EVT_N0183_GGA, this, n0183_msg_GGA);

  Bind(EVT_N0183_GGA, [&](ObservedEvt ev) {
    HandleN0183_GGA(UnpackEvtPointer<Nmea0183Msg>(ev));
  });

  // GLL
  Nmea0183Msg n0183_msg_GLL("GLL");
  listener_N0183_GLL = msgbus.GetListener(EVT_N0183_GLL, this, n0183_msg_GLL);

  Bind(EVT_N0183_GLL, [&](ObservedEvt ev) {
    HandleN0183_GLL(UnpackEvtPointer<Nmea0183Msg>(ev));
  });

  // AIVDO
  Nmea0183Msg n0183_msg_AIVDO("AIVDO");
  listener_N0183_AIVDO =
      msgbus.GetListener(EVT_N0183_AIVDO, this, n0183_msg_AIVDO);

  Bind(EVT_N0183_AIVDO, [&](ObservedEvt ev) {
    HandleN0183_AIVDO(UnpackEvtPointer<Nmea0183Msg>(ev));
  });

  // SignalK
  SignalkMsg sk_msg;
  listener_SignalK =
      msgbus.GetListener(EVT_SIGNALK, this, sk_msg);

  Bind(EVT_SIGNALK, [&](ObservedEvt ev) {
    HandleSignalK(UnpackEvtPointer<SignalkMsg>(ev));
  });

}

void CommBridge::OnDriverStateChange(){

  // Reset all "first-come" priority states
  InitializePriorityContainers();

  g_bHDT_Rx = false;
}


bool CommBridge::HandleN2K_129029(std::shared_ptr<const Nmea2000Msg> n2k_msg) {
  //printf("   HandleN2K_129029\n");

  std::vector<unsigned char> v = n2k_msg->payload;

  // extract and verify PGN
  uint64_t pgn = 0;
  unsigned char *c = (unsigned char *)&pgn;
  *c++ = v.at(3);
  *c++ = v.at(4);
  *c++ = v.at(5);

  NavData temp_data;
  ClearNavData(temp_data);

  if (!m_decoder.DecodePGN129029(v, temp_data))
    return false;

  if (!N2kIsNA(temp_data.gLat) && !N2kIsNA(temp_data.gLon)){
    if (EvalPriority(n2k_msg, active_priority_position, priority_map_position)) {
      gLat = temp_data.gLat;
      gLon = temp_data.gLon;
      m_watchdogs.position_watchdog = gps_watchdog_timeout_ticks;
    }
  }

   if (temp_data.n_satellites >= 0){
     if (EvalPriority(n2k_msg, active_priority_satellites, priority_map_satellites)) {
       g_SatsInView = temp_data.n_satellites;
       g_bSatValid = true;
       m_watchdogs.satellite_watchdog = sat_watchdog_timeout_ticks;
     }
   }

    // Populate a comm_appmsg with current global values
  auto msg = std::make_shared<BasicNavDataMsg>(
      gLat, gLon, gSog, gCog, gVar, gHdt, wxDateTime::Now().GetTicks());

  // Notify the AppMsgBus of new data available
  auto& msgbus = AppMsgBus::GetInstance();
  msgbus.Notify(std::move(msg));

  return true;
}

bool CommBridge::HandleN2K_129025(std::shared_ptr<const Nmea2000Msg> n2k_msg) {

  std::vector<unsigned char> v = n2k_msg->payload;

  NavData temp_data;
  ClearNavData(temp_data);

  if (!m_decoder.DecodePGN129025(v, temp_data))
    return false;

  if (!N2kIsNA(temp_data.gLat) && !N2kIsNA(temp_data.gLon)){
    if (EvalPriority(n2k_msg, active_priority_position, priority_map_position)) {
      gLat = temp_data.gLat;
      gLon = temp_data.gLon;
      m_watchdogs.position_watchdog = gps_watchdog_timeout_ticks;
    }
  }
  //FIXME (dave) How to notify user of errors?
  else{
  }

    // Populate a comm_appmsg with current global values
  auto msg = std::make_shared<BasicNavDataMsg>(
      gLat, gLon, gSog, gCog, gVar, gHdt, wxDateTime::Now().GetTicks());

  // Notify the AppMsgBus of new data available
  auto& msgbus = AppMsgBus::GetInstance();
  msgbus.Notify(std::move(msg));

  return true;
}

bool CommBridge::HandleN2K_129026(std::shared_ptr<const Nmea2000Msg> n2k_msg) {

  std::vector<unsigned char> v = n2k_msg->payload;

  NavData temp_data;
  ClearNavData(temp_data);

  if (!m_decoder.DecodePGN129026(v, temp_data))
    return false;

  if (!N2kIsNA(temp_data.gSog) && !N2kIsNA(temp_data.gCog)){

    // Require COG/SOG to come from the same device as is providing position.
    //Check the Message Source Address.
    //If same as Source Address of highest position priority, then accept it.

    unsigned char n_source = n2k_msg->payload.at(7);

    if( n_source == active_priority_position.active_source_address){
      gSog = MS2KNOTS(temp_data.gSog);
      gCog = GeodesicRadToDeg(temp_data.gCog);
      m_watchdogs.velocity_watchdog = gps_watchdog_timeout_ticks;
    }
  }
  else{
  }

    // Populate a comm_appmsg with current global values
  auto msg = std::make_shared<BasicNavDataMsg>(
      gLat, gLon, gSog, gCog, gVar, gHdt, wxDateTime::Now().GetTicks());

  // Notify the AppMsgBus of new data available
  auto& msgbus = AppMsgBus::GetInstance();
  msgbus.Notify(std::move(msg));

  return true;
}

bool CommBridge::HandleN2K_127250(std::shared_ptr<const Nmea2000Msg> n2k_msg) {

  std::vector<unsigned char> v = n2k_msg->payload;

  NavData temp_data;
  ClearNavData(temp_data);

  if (!m_decoder.DecodePGN127250(v, temp_data))
    return false;

  if (!N2kIsNA(temp_data.gHdt)){
    if (EvalPriority(n2k_msg, active_priority_heading, priority_map_heading)) {
      gHdt = GeodesicRadToDeg(temp_data.gHdt);
      m_watchdogs.heading_watchdog = gps_watchdog_timeout_ticks;
    }
  }

  if (!N2kIsNA(temp_data.gVar)){
    if (EvalPriority(n2k_msg, active_priority_variation, priority_map_variation)) {
      gVar = GeodesicRadToDeg(temp_data.gVar);
      m_watchdogs.variation_watchdog = gps_watchdog_timeout_ticks;
    }
  }

    // Populate a comm_appmsg with current global values
  auto msg = std::make_shared<BasicNavDataMsg>(
      gLat, gLon, gSog, gCog, gVar, gHdt, wxDateTime::Now().GetTicks());

  // Notify the AppMsgBus of new data available
  auto& msgbus = AppMsgBus::GetInstance();
  msgbus.Notify(std::move(msg));

  return true;
}

bool CommBridge::HandleN2K_129540(std::shared_ptr<const Nmea2000Msg> n2k_msg) {

  std::vector<unsigned char> v = n2k_msg->payload;

  NavData temp_data;
  ClearNavData(temp_data);

  if (!m_decoder.DecodePGN129540(v, temp_data))
    return false;

   if (temp_data.n_satellites >= 0){
    if (EvalPriority(n2k_msg, active_priority_satellites, priority_map_satellites)) {
      g_SatsInView = temp_data.n_satellites;
      g_bSatValid = true;
      m_watchdogs.satellite_watchdog = sat_watchdog_timeout_ticks;
    }
  }

  return true;
}

bool CommBridge::HandleN0183_RMC(std::shared_ptr<const Nmea0183Msg> n0183_msg) {
  std::string str = n0183_msg->payload;

  NavData temp_data;
  ClearNavData(temp_data);

  if (!m_decoder.DecodeRMC(str, temp_data))
    return false;

  if (EvalPriority(n0183_msg, active_priority_position, priority_map_position)) {
    gLat = temp_data.gLat;
    gLon = temp_data.gLon;
    m_watchdogs.position_watchdog = gps_watchdog_timeout_ticks;
  }

  if (EvalPriority(n0183_msg, active_priority_velocity, priority_map_velocity)) {
    gSog = temp_data.gSog;
    gCog = temp_data.gCog;
    m_watchdogs.velocity_watchdog = gps_watchdog_timeout_ticks;
  }

  if (!std::isnan(temp_data.gVar)){
    if (EvalPriority(n0183_msg, active_priority_variation, priority_map_variation)) {
      gVar = temp_data.gVar;
      m_watchdogs.variation_watchdog = gps_watchdog_timeout_ticks;
    }
  }

  // Populate a comm_appmsg with current global values
  auto msg = std::make_shared<BasicNavDataMsg>(
      gLat, gLon, gSog, gCog, gVar, gHdt, wxDateTime::Now().GetTicks());

  // Notify the AppMsgBus of new data available
  auto& msgbus = AppMsgBus::GetInstance();
  msgbus.Notify(std::move(msg));

  return true;
}

bool CommBridge::HandleN0183_HDT(std::shared_ptr<const Nmea0183Msg> n0183_msg) {
  std::string str = n0183_msg->payload;
  NavData temp_data;
  ClearNavData(temp_data);

  if (!m_decoder.DecodeHDT(str, temp_data))
    return false;

  if (EvalPriority(n0183_msg, active_priority_heading, priority_map_heading)) {
     gHdt = temp_data.gHdt;
     m_watchdogs.heading_watchdog = gps_watchdog_timeout_ticks;
  }


  // Populate a comm_appmsg with current global values
  auto msg = std::make_shared<BasicNavDataMsg>(
      gLat, gLon, gSog, gCog, gVar, gHdt, wxDateTime::Now().GetTicks());

  // Notify the AppMsgBus of new data available
  auto& msgbus = AppMsgBus::GetInstance();
  msgbus.Notify(std::move(msg));

  return true;
}

bool CommBridge::HandleN0183_HDG(std::shared_ptr<const Nmea0183Msg> n0183_msg) {
  std::string str = n0183_msg->payload;
  NavData temp_data;
  ClearNavData(temp_data);

  if (!m_decoder.DecodeHDG(str, temp_data)) return false;

  bool bHDM = false;
  if (EvalPriority(n0183_msg, active_priority_heading, priority_map_heading)) {
     gHdm = temp_data.gHdm;
     m_watchdogs.heading_watchdog = gps_watchdog_timeout_ticks;
     bHDM = true;
  }

  if (!std::isnan(temp_data.gVar)){
    if (EvalPriority(n0183_msg, active_priority_variation, priority_map_variation)) {
      gVar = temp_data.gVar;
      m_watchdogs.variation_watchdog = gps_watchdog_timeout_ticks;
    }
  }

  if (bHDM)
    MakeHDTFromHDM();


  // Populate a comm_appmsg with current global values
  auto msg = std::make_shared<BasicNavDataMsg>(
      gLat, gLon, gSog, gCog, gVar, gHdt, wxDateTime::Now().GetTicks());

  // Notify the AppMsgBus of new data available
  auto& msgbus = AppMsgBus::GetInstance();
  msgbus.Notify(std::move(msg));

  return true;
}

bool CommBridge::HandleN0183_HDM(std::shared_ptr<const Nmea0183Msg> n0183_msg) {
  std::string str = n0183_msg->payload;
  NavData temp_data;
  ClearNavData(temp_data);

  if (!m_decoder.DecodeHDM(str, temp_data)) return false;

  if (EvalPriority(n0183_msg, active_priority_heading, priority_map_heading)) {
    gHdm = temp_data.gHdm;
    MakeHDTFromHDM();
    m_watchdogs.heading_watchdog = gps_watchdog_timeout_ticks;
  }

  // Populate a comm_appmsg with current global values
  auto msg = std::make_shared<BasicNavDataMsg>(
      gLat, gLon, gSog, gCog, gVar, gHdt, wxDateTime::Now().GetTicks());

  // Notify the AppMsgBus of new data available
  auto& msgbus = AppMsgBus::GetInstance();
  msgbus.Notify(std::move(msg));

  return true;
}

bool CommBridge::HandleN0183_VTG(std::shared_ptr<const Nmea0183Msg> n0183_msg) {
  std::string str = n0183_msg->payload;
  NavData temp_data;
  ClearNavData(temp_data);

  if (!m_decoder.DecodeVTG(str, temp_data)) return false;

    if (EvalPriority(n0183_msg, active_priority_velocity, priority_map_velocity)) {
    gSog = temp_data.gSog;
    gCog = temp_data.gCog;
    m_watchdogs.velocity_watchdog = gps_watchdog_timeout_ticks;
  }

  // Populate a comm_appmsg with current global values
  auto msg = std::make_shared<BasicNavDataMsg>(
      gLat, gLon, gSog, gCog, gVar, gHdt, wxDateTime::Now().GetTicks());

  // Notify the AppMsgBus of new data available
  auto& msgbus = AppMsgBus::GetInstance();
  msgbus.Notify(std::move(msg));

  return true;
}

bool CommBridge::HandleN0183_GSV(std::shared_ptr<const Nmea0183Msg> n0183_msg) {
  std::string str = n0183_msg->payload;
  NavData temp_data;
  ClearNavData(temp_data);

  if (!m_decoder.DecodeGSV(str, temp_data)) return false;

  if (EvalPriority(n0183_msg, active_priority_satellites, priority_map_satellites)) {
    if (temp_data.n_satellites >= 0){
      g_SatsInView = temp_data.n_satellites;
      g_bSatValid = true;

      m_watchdogs.satellite_watchdog = sat_watchdog_timeout_ticks;
    }
  }

  // Populate a comm_appmsg with current global values
  auto msg = std::make_shared<BasicNavDataMsg>(
      gLat, gLon, gSog, gCog, gVar, gHdt, wxDateTime::Now().GetTicks());

  // Notify the AppMsgBus of new data available
  auto& msgbus = AppMsgBus::GetInstance();
  msgbus.Notify(std::move(msg));

  return true;
}

bool CommBridge::HandleN0183_GGA(std::shared_ptr<const Nmea0183Msg> n0183_msg) {
  std::string str = n0183_msg->payload;
  NavData temp_data;
  ClearNavData(temp_data);

  if (!m_decoder.DecodeGGA(str, temp_data)) return false;

  if (EvalPriority(n0183_msg, active_priority_position, priority_map_position)) {
     gLat = temp_data.gLat;
     gLon = temp_data.gLon;
     m_watchdogs.position_watchdog = gps_watchdog_timeout_ticks;
  }

  if (EvalPriority(n0183_msg, active_priority_satellites, priority_map_satellites)) {
    if (temp_data.n_satellites >= 0){
      g_SatsInView = temp_data.n_satellites;
      g_bSatValid = true;

      m_watchdogs.satellite_watchdog = sat_watchdog_timeout_ticks;
    }
  }

  // Populate a comm_appmsg with current global values
  auto msg = std::make_shared<BasicNavDataMsg>(
      gLat, gLon, gSog, gCog, gVar, gHdt, wxDateTime::Now().GetTicks());

  // Notify the AppMsgBus of new data available
  auto& msgbus = AppMsgBus::GetInstance();
  msgbus.Notify(std::move(msg));

  return true;
}

bool CommBridge::HandleN0183_GLL(std::shared_ptr<const Nmea0183Msg> n0183_msg) {
  std::string str = n0183_msg->payload;
  NavData temp_data;
  ClearNavData(temp_data);

  if (!m_decoder.DecodeGLL(str, temp_data)) return false;

  if (EvalPriority(n0183_msg, active_priority_position, priority_map_position)) {
     gLat = temp_data.gLat;
     gLon = temp_data.gLon;
     m_watchdogs.position_watchdog = gps_watchdog_timeout_ticks;
  }

  // Populate a comm_appmsg with current global values
  auto msg = std::make_shared<BasicNavDataMsg>(
      gLat, gLon, gSog, gCog, gVar, gHdt, wxDateTime::Now().GetTicks());

  // Notify the AppMsgBus of new data available
  auto& msgbus = AppMsgBus::GetInstance();
  msgbus.Notify(std::move(msg));

  return true;
}

bool CommBridge::HandleN0183_AIVDO(
  std::shared_ptr<const Nmea0183Msg> n0183_msg) {
  std::string str = n0183_msg->payload;

  GenericPosDatEx gpd;
  wxString sentence(str.c_str());

  AisError nerr = AIS_GENERIC_ERROR;
  nerr = DecodeSingleVDO(sentence, &gpd);

  if (nerr == AIS_NoError) {

    if (!std::isnan(gpd.kLat) && !std::isnan(gpd.kLon)){
      if (EvalPriority(n0183_msg, active_priority_position, priority_map_position)) {
        gLat = gpd.kLat;
        gLon = gpd.kLon;
        m_watchdogs.position_watchdog = gps_watchdog_timeout_ticks;
      }
    }

    if (!std::isnan(gpd.kCog) && !std::isnan(gpd.kSog)){
      if (EvalPriority(n0183_msg, active_priority_velocity, priority_map_velocity)) {
        gSog = gpd.kSog;
        gCog = gpd.kCog;
        m_watchdogs.velocity_watchdog = gps_watchdog_timeout_ticks;
      }
    }

    if (!std::isnan(gpd.kHdt)) {
      if (EvalPriority(n0183_msg, active_priority_heading, priority_map_heading)) {
        gHdt = gpd.kHdt;
        m_watchdogs.heading_watchdog = gps_watchdog_timeout_ticks;
      }
    }

    // Populate a comm_appmsg with current global values
    auto msg = std::make_shared<BasicNavDataMsg>(
      gLat, gLon, gSog, gCog, gVar, gHdt, wxDateTime::Now().GetTicks());

    // Notify the AppMsgBus of new data available
    auto& msgbus = AppMsgBus::GetInstance();
    msgbus.Notify(std::move(msg));

  }
  return true;
}

bool CommBridge::HandleSignalK(std::shared_ptr<const SignalkMsg> sK_msg){
  std::string str = sK_msg->raw_message;

  //  Here we ignore messages involving contexts other than ownship
  if (sK_msg->context_self != sK_msg->context)
    return false;

  //g_ownshipMMSI_SK = sK_msg->context_self;

  NavData temp_data;
  ClearNavData(temp_data);

  if (!m_decoder.DecodeSignalK(str, temp_data)) return false;


  if (!std::isnan(temp_data.gLat) && !std::isnan(temp_data.gLon)){
    if (EvalPriority(sK_msg, active_priority_position, priority_map_position)) {
      gLat = temp_data.gLat;
      gLon = temp_data.gLon;
      m_watchdogs.position_watchdog = gps_watchdog_timeout_ticks;
    }
  }

  if (!std::isnan(temp_data.gSog)){
    if (EvalPriority(sK_msg, active_priority_velocity, priority_map_velocity)) {
    gSog = temp_data.gSog;
    if((gSog > 0) && !std::isnan(temp_data.gCog))
      gCog = temp_data.gCog;
    m_watchdogs.velocity_watchdog = gps_watchdog_timeout_ticks;
    }
  }

  if (!std::isnan(temp_data.gHdt)){
    if (EvalPriority(sK_msg, active_priority_heading, priority_map_heading)) {
      gHdt = temp_data.gHdt;
      m_watchdogs.heading_watchdog = gps_watchdog_timeout_ticks;
    }
  }

  if (!std::isnan(temp_data.gHdm)){
    if (EvalPriority(sK_msg, active_priority_heading, priority_map_heading)) {
      gHdm = temp_data.gHdm;
      MakeHDTFromHDM();
      m_watchdogs.heading_watchdog = gps_watchdog_timeout_ticks;
    }
  }

  if (!std::isnan(temp_data.gVar)){
    if (EvalPriority(sK_msg, active_priority_variation, priority_map_variation)) {
      gVar = temp_data.gVar;
      m_watchdogs.variation_watchdog = gps_watchdog_timeout_ticks;
    }
  }

  if (temp_data.n_satellites > 0){
    if (EvalPriority(sK_msg, active_priority_satellites, priority_map_satellites)) {
      g_SatsInView = temp_data.n_satellites;
      g_bSatValid = true;

      m_watchdogs.satellite_watchdog = sat_watchdog_timeout_ticks;
    }
  }

  // Populate a comm_appmsg with current global values
  auto msg = std::make_shared<BasicNavDataMsg>(
      gLat, gLon, gSog, gCog, gVar, gHdt, wxDateTime::Now().GetTicks());

  // Notify the AppMsgBus of new data available
  auto& msgbus = AppMsgBus::GetInstance();
  msgbus.Notify(std::move(msg));


  return true;
}


void CommBridge::InitializePriorityContainers(){
  active_priority_position.active_priority = 0;
  active_priority_velocity.active_priority = 0;
  active_priority_heading.active_priority = 0;
  active_priority_variation.active_priority = 0;
  active_priority_satellites.active_priority = 0;

  active_priority_position.active_source.clear();
  active_priority_velocity.active_source.clear();
  active_priority_heading.active_source.clear();
  active_priority_variation.active_source.clear();
  active_priority_satellites.active_source.clear();

  active_priority_position.active_identifier.clear();
  active_priority_velocity.active_identifier.clear();
  active_priority_heading.active_identifier.clear();
  active_priority_variation.active_identifier.clear();
  active_priority_satellites.active_identifier.clear();

  active_priority_position.pcclass = "position";
  active_priority_velocity.pcclass = "velocity";
  active_priority_heading.pcclass = "heading";
  active_priority_variation.pcclass = "variation";
  active_priority_satellites.pcclass = "satellites";

  active_priority_position.active_source_address = -1;
  active_priority_velocity.active_source_address = -1;
  active_priority_heading.active_source_address = -1;
  active_priority_variation.active_source_address = -1;
  active_priority_satellites.active_source_address = -1;

  priority_map_position.clear();
  priority_map_velocity.clear();
  priority_map_heading.clear();
  priority_map_variation.clear();
  priority_map_satellites.clear();
}

std::string CommBridge::GetPriorityKey(std::shared_ptr <const NavMsg> msg){
  std::string source = msg->source->to_string();
  std::string listener_key = msg->key();


  std::string this_identifier;
  std::string this_address("0");
  if(msg->bus == NavAddr::Bus::N0183){
    auto msg_0183 = std::dynamic_pointer_cast<const Nmea0183Msg>(msg);
    if (msg_0183){
      this_identifier = msg_0183->talker;
      this_identifier += msg_0183->type;
    }
  }
  else if(msg->bus == NavAddr::Bus::N2000){
    auto msg_n2k = std::dynamic_pointer_cast<const Nmea2000Msg>(msg);
    if (msg_n2k){
      this_identifier = msg_n2k->name.to_string();
      unsigned char n_source = msg_n2k->payload.at(7);
      char ss[4];
      sprintf(ss,"%d",n_source);
      this_address = std::string(ss);
    }
  }
  else if(msg->bus == NavAddr::Bus::Signalk){
    auto msg_sk = std::dynamic_pointer_cast<const SignalkMsg>(msg);
    if (msg_sk){
      this_identifier = "signalK";
    }
  }


  return  source + ":" + this_address + ";" + this_identifier;

}

bool CommBridge::EvalPriority(std::shared_ptr <const NavMsg> msg,
                                      PriorityContainer& active_priority,
                                      std::unordered_map<std::string, int>& priority_map) {

  std::string this_key = GetPriorityKey(msg);
  if (debug_priority) printf("This Key: %s\n", this_key.c_str());

  // Pull some identifiers from the unique key
  wxStringTokenizer tkz(this_key, _T(";"));
  wxString wxs_this_source = tkz.GetNextToken();
  std::string source = wxs_this_source.ToStdString();
  wxString wxs_this_identifier = tkz.GetNextToken();
  std::string this_identifier = wxs_this_identifier.ToStdString();

  wxStringTokenizer tka(wxs_this_source, _T(":"));
  tka.GetNextToken();
  int source_address = atoi(tka.GetNextToken().ToStdString().c_str());

  // Fetch the established priority for the message
  int this_priority;

  auto it = priority_map.find(this_key);
  if (it == priority_map.end()) {
    // Not found, so make it default highest priority
    priority_map[this_key] = 0;
  }

  this_priority = priority_map[this_key];

  for (auto it = priority_map.begin(); it != priority_map.end(); it++) {
        if (debug_priority) printf("               priority_map:  %s  %d\n", it->first.c_str(), it->second);
  }

  //Incoming message priority lower than currently active priority?
  //  If so, drop the message
  if (this_priority > active_priority.active_priority){
    if (debug_priority)  printf("      Drop low priority: %s %d %d \n", source.c_str(), this_priority,
                                  active_priority.active_priority);
    return false;
  }

  // A channel returning, after being watchdogged out.
  if (this_priority < active_priority.active_priority){
    active_priority.active_priority = this_priority;
    active_priority.active_source = source;
    active_priority.active_identifier = this_identifier;
    active_priority.active_source_address = source_address;

    if (debug_priority) printf("  Restoring high priority: %s %d\n", source.c_str(), this_priority);
    return true;
  }


  // Do we see two sources with the same priority?
  // If so, we take the first one, and deprioritize this one.

  if (active_priority.active_source.size()){

    if (debug_priority) printf("source: %s\n", source.c_str());
    if (debug_priority) printf("active_source: %s\n", active_priority.active_source.c_str());

    if (source.compare(active_priority.active_source) != 0){

      // Auto adjust the priority of the this message down
      //First, find the lowest priority in use in this map
      int lowest_priority = -10;     // safe enough
      for (auto it = priority_map.begin(); it != priority_map.end(); it++) {
        if (it->second > lowest_priority)
          lowest_priority = it->second;
      }

      priority_map[this_key] = lowest_priority + 1;
      if (debug_priority) printf("          Lowering priority A: %s :%d\n", source.c_str(), priority_map[this_key]);
      return false;
    }
  }

  //  For N0183 message, has the Mnemonic (id) changed?
  //  Example:  RMC and AIVDO from same source.


  if(msg->bus == NavAddr::Bus::N0183){
    auto msg_0183 = std::dynamic_pointer_cast<const Nmea0183Msg>(msg);
    if (msg_0183){
      if (active_priority.active_identifier.size()){

        if (debug_priority) printf("this_identifier: %s\n", this_identifier.c_str());
        if (debug_priority) printf("active_priority.active_identifier: %s\n", active_priority.active_identifier.c_str());

        if (this_identifier.compare(active_priority.active_identifier) != 0){
          // if necessary, auto adjust the priority of the this message down
          //and drop it
          if (priority_map[this_key] == active_priority.active_priority){
            int lowest_priority = -10;     // safe enough
            for (auto it = priority_map.begin(); it != priority_map.end(); it++) {
              if (it->second > lowest_priority)
                lowest_priority = it->second;
            }

            priority_map[this_key] = lowest_priority + 1;
            if (debug_priority) printf("          Lowering priority B: %s :%d\n", source.c_str(), priority_map[this_key]);
          }

          return false;
        }
      }
    }
  }

  //  Similar for n2k PGN...

  else if(msg->bus == NavAddr::Bus::N2000){
    auto msg_n2k = std::dynamic_pointer_cast<const Nmea2000Msg>(msg);
    if (msg_n2k){
      if (active_priority.active_identifier.size()){
        if (this_identifier.compare(active_priority.active_identifier) != 0){
         // if necessary, auto adjust the priority of the this message down
          //and drop it
          if (priority_map[this_key] == active_priority.active_priority){
            int lowest_priority = -10;     // safe enough
            for (auto it = priority_map.begin(); it != priority_map.end(); it++) {
              if (it->second > lowest_priority)
                lowest_priority = it->second;
            }

            priority_map[this_key] = lowest_priority + 1;
            if (debug_priority) printf("          Lowering priority: %s :%d\n", source.c_str(), priority_map[this_key]);
          }

          return false;
        }
      }
    }
  }


  // Update the records
  active_priority.active_source = source;
  active_priority.active_identifier = this_identifier;
  active_priority.active_source_address = source_address;
  if (debug_priority) printf("  Accepting high priority: %s %d\n", source.c_str(), this_priority);

  return true;
}