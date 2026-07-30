#include <algorithm>
#include "toshiba_climate.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome { namespace toshiba_suzumi {
static const uint8_t REGS[] = {0x80,0x81,0x82,0x86,0x87,0x88,0x89,0x90,0x92,0x96,0x97,0x98,0x99,0x9A,0xA4,0xB4,0xB7,0xB9,0xBA,0xBB,0xBE,0xC0,0xC6,0xC7,0xCA,0xD4,0xD7,0xDA,0xE2,0xE3,0xE4,0xE5,0xEE,0xF8,0xFE};
static const size_t NREGS=sizeof(REGS)/sizeof(REGS[0]);
static_assert(NREGS==35,"monitor register count");
static const uint32_t CYCLE_MS=60000, TIMEOUT_MS=1000, B7_TIMEOUT_MS=2000, QUIET_MS=250;
static const size_t CHUNK=24;
static uint8_t msum(const std::vector<uint8_t>&v){uint8_t s=0;for(size_t i=1;i<v.size();i++)s+=v[i];return 256-s;}
static bool payload(const std::vector<uint8_t>&r,int16_t reg,size_t&o,size_t&n){size_t z=r.size();if(z<2||reg<0)return false;size_t p=z;if((z==15||z==22)&&r[12]==reg)p=12;else if(z>14&&r[3]==0x90&&r[14]==reg)p=14;else if(z>12&&r[12]==reg)p=12;if(p==z)return false;o=p+1;n=(z-1)-o;return o<=z-1;}
static uint32_t le32(const uint8_t*p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}

void ToshibaDiagnosticMonitorUart::loop(){
  while(available()){uint8_t c;read_byte(&c);handle_rx_byte_(c);}
  if(!(scan_active_&&scan_request_sent_&&scan_register_==0xB7&&!rx_message_.empty()))process_command_queue_();
  process_scan_();
}

void ToshibaDiagnosticMonitorUart::set_scan_enabled(bool on){
  if(on){
    if(scan_active_){monitor_stop_requested_=false;return;}
    scan_active_=true;scan_started_=false;scan_request_sent_=false;scan_matched_response_=false;
    monitor_stop_requested_=monitor_waiting_for_cycle_=monitor_b7_retry_pending_=monitor_b7_retry_active_=false;
    monitor_register_index_=0;monitor_cycle_number_=monitor_requests_=monitor_registers_attempted_=monitor_matched_=monitor_timeouts_=monitor_unrelated_=monitor_cycles_completed_=0;
    monitor_b7_first_timeouts_=monitor_b7_retries_=monitor_b7_retry_matches_=monitor_b7_retry_timeouts_=0;
    monitor_da_previous_valid_=false;monitor_b7_partial_.clear();monitor_last_unrelated_packet_.clear();
    ESP_LOGI(TAG,"========== TOSHIBA ONE-MINUTE DIAGNOSTIC MONITOR STARTED ==========");
    ESP_LOGI(TAG,"registers=35 cadence=60s read_only=YES B7_retry=YES");
    start_monitor_cycle_(millis());return;
  }
  if(!scan_active_)return;monitor_stop_requested_=true;
  if(!scan_request_sent_)finish_monitor_();
}

void ToshibaDiagnosticMonitorUart::reset_monitor_cycle_state_(){
  monitor_cycle_attempted_=monitor_cycle_matched_=monitor_cycle_timeouts_=monitor_cycle_requests_=0;
  monitor_cycle_unrelated_=monitor_cycle_unrelated_repeated_=monitor_cycle_unrelated_changed_=0;
  monitor_power_valid_=monitor_limit_valid_=monitor_room_valid_=monitor_outdoor_valid_=false;
  monitor_f8_valid_=monitor_e4_valid_=monitor_e5_valid_=false;monitor_last_unrelated_packet_.clear();
}
void ToshibaDiagnosticMonitorUart::start_monitor_cycle_(uint32_t now){
  monitor_cycle_started_=now;monitor_cycle_number_++;monitor_register_index_=0;monitor_waiting_for_cycle_=false;
  monitor_b7_retry_pending_=monitor_b7_retry_active_=false;monitor_b7_partial_.clear();reset_monitor_cycle_state_();
  ESP_LOGI(TAG,"MONITOR cycle=%u started",(unsigned)monitor_cycle_number_);
}

void ToshibaDiagnosticMonitorUart::process_scan_(){
  if(!scan_active_)return;uint32_t now=millis();
  if(monitor_stop_requested_&&!scan_request_sent_){finish_monitor_();return;}
  if(monitor_waiting_for_cycle_){if(now-monitor_cycle_started_<CYCLE_MS)return;start_monitor_cycle_(now);}
  if(monitor_b7_retry_pending_){
    if(now-last_command_timestamp_<QUIET_MS)return;
    monitor_b7_retry_pending_=false;monitor_b7_retry_active_=true;scan_matched_response_=false;scan_last_packet_timestamp_=0;
    scan_register_started_=now;scan_request_sent_=true;monitor_b7_retries_++;monitor_b7_partial_.clear();
    ESP_LOGI(TAG,"MONITOR cycle=%u B7 retry started",(unsigned)monitor_cycle_number_);send_monitor_request_();return;
  }
  if(!scan_request_sent_){
    if((!scan_started_&&!command_queue_.empty())||!rx_message_.empty()||now-last_command_timestamp_<QUIET_MS)return;
    scan_register_=REGS[monitor_register_index_];scan_matched_response_=false;scan_last_packet_timestamp_=0;
    scan_register_started_=now;scan_started_=true;scan_request_sent_=true;send_monitor_request_();return;
  }
  if(scan_matched_response_){if(now-scan_last_packet_timestamp_>=QUIET_MS)complete_monitor_request_();return;}
  uint32_t t=scan_register_==0xB7?B7_TIMEOUT_MS:TIMEOUT_MS;if(now-scan_register_started_>=t)handle_monitor_timeout_();
}

void ToshibaDiagnosticMonitorUart::send_monitor_request_(){
  std::vector<uint8_t> p={2,0,3,16,0,0,6,1,48,1,0,1};p.push_back(scan_register_);p.push_back(msum(p));
  monitor_requests_++;monitor_cycle_requests_++;send_to_uart(ToshibaCommand{static_cast<ToshibaCommandType>(scan_register_),p,0});
}
void ToshibaDiagnosticMonitorUart::handle_monitor_timeout_(){
  if(scan_register_==0xB7&&!rx_message_.empty()){monitor_b7_partial_=rx_message_;rx_message_.clear();log_monitor_b7_partial_();}
  if(scan_register_==0xB7&&!monitor_b7_retry_active_){monitor_b7_first_timeouts_++;monitor_b7_retry_pending_=true;scan_request_sent_=false;ESP_LOGI(TAG,"MONITOR cycle=%u B7 first timeout; retry pending",(unsigned)monitor_cycle_number_);return;}
  if(scan_register_==0xB7)monitor_b7_retry_timeouts_++;complete_monitor_request_();
}
void ToshibaDiagnosticMonitorUart::complete_monitor_request_(){
  monitor_registers_attempted_++;monitor_cycle_attempted_++;
  if(scan_matched_response_){monitor_matched_++;monitor_cycle_matched_++;if(scan_register_==0xB7&&monitor_b7_retry_active_)monitor_b7_retry_matches_++;}
  else{monitor_timeouts_++;monitor_cycle_timeouts_++;ESP_LOGI(TAG,"MONITOR cycle=%u reg=0x%02X timeout",(unsigned)monitor_cycle_number_,(unsigned)scan_register_);}
  scan_request_sent_=false;scan_matched_response_=false;monitor_b7_retry_active_=false;rx_message_.clear();
  if(monitor_stop_requested_){finish_monitor_();return;}monitor_register_index_++;
  if(monitor_register_index_>=NREGS){monitor_cycles_completed_++;monitor_waiting_for_cycle_=true;log_monitor_cycle_summary_();}
}
void ToshibaDiagnosticMonitorUart::finish_monitor_(){
  if(!scan_active_)return;scan_active_=scan_started_=scan_request_sent_=scan_matched_response_=false;
  monitor_stop_requested_=monitor_waiting_for_cycle_=monitor_b7_retry_pending_=monitor_b7_retry_active_=false;rx_message_.clear();
  ESP_LOGI(TAG,"MONITOR stopped registers=%u requests=%u matched=%u timeouts=%u unrelated=%u cycles=%u",(unsigned)monitor_registers_attempted_,(unsigned)monitor_requests_,(unsigned)monitor_matched_,(unsigned)monitor_timeouts_,(unsigned)monitor_unrelated_,(unsigned)monitor_cycles_completed_);
  ESP_LOGI(TAG,"MONITOR B7 first_timeouts=%u retries=%u retry_matches=%u retry_timeouts=%u",(unsigned)monitor_b7_first_timeouts_,(unsigned)monitor_b7_retries_,(unsigned)monitor_b7_retry_matches_,(unsigned)monitor_b7_retry_timeouts_);getInitData();
}

void ToshibaDiagnosticMonitorUart::log_scan_packet_(const std::vector<uint8_t>&r){
  int16_t reg=extract_response_register_(r);bool match=reg==scan_register_;scan_last_packet_timestamp_=millis();
  if(match)scan_matched_response_=true;else{monitor_unrelated_++;monitor_cycle_unrelated_++;if(r==monitor_last_unrelated_packet_){monitor_cycle_unrelated_repeated_++;return;}monitor_cycle_unrelated_changed_++;monitor_last_unrelated_packet_=r;ESP_LOGI(TAG,"MONITOR UNSOLICITED request=0x%02X response=%d length=%u DATA=[%s]",(unsigned)scan_register_,(int)reg,(unsigned)r.size(),format_hex_pretty(r).c_str());return;}
  ESP_LOGI(TAG,"MONITOR cycle=%u reg=0x%02X attempt=%s length=%u checksum=OK",(unsigned)monitor_cycle_number_,(unsigned)reg,(reg==0xB7&&monitor_b7_retry_active_)?"retry":"first",(unsigned)r.size());
  capture_monitor_control_(r,reg);size_t o,n;if(reg!=0xDA&&payload(r,reg,o,n))ESP_LOGI(TAG,"MONITOR cycle=%u reg=0x%02X payload=[%s]",(unsigned)monitor_cycle_number_,(unsigned)reg,format_hex_pretty(r.data()+o,n).c_str());
  if(reg==0xDA){log_monitor_bytes_(r,reg);log_monitor_da_(r);}else if(reg==0xB7)log_monitor_bytes_(r,reg);
}
void ToshibaDiagnosticMonitorUart::capture_monitor_control_(const std::vector<uint8_t>&r,int16_t reg){
  size_t o,n;if(!payload(r,reg,o,n))return;const uint8_t*p=r.data()+o;
  if(n==1){ESP_LOGI(TAG,"MONITOR VALUE reg=0x%02X unsigned=%u signed=%d",(unsigned)reg,(unsigned)p[0],(int)(int8_t)p[0]);if(reg==0x80){monitor_power_=p[0];monitor_power_valid_=true;}if(reg==0x87){monitor_limit_=p[0];monitor_limit_valid_=true;}if(reg==0xBB){monitor_room_=p[0];monitor_room_valid_=true;}if(reg==0xBE){monitor_outdoor_=p[0];monitor_outdoor_valid_=true;}}
  if(reg==0xF8&&n>=4){std::copy(p,p+4,monitor_f8_);monitor_f8_valid_=true;}if(reg==0xE4&&n>=3){std::copy(p,p+3,monitor_e4_);monitor_e4_valid_=true;}if(reg==0xE5&&n>=8){std::copy(p,p+8,monitor_e5_);monitor_e5_valid_=true;}
}
void ToshibaDiagnosticMonitorUart::log_monitor_cycle_summary_()const{
  ESP_LOGI(TAG,"MONITOR cycle=%u complete attempted=%u/35 requests=%u matched=%u timeouts=%u unsolicited=%u changed=%u repeated=%u",(unsigned)monitor_cycle_number_,(unsigned)monitor_cycle_attempted_,(unsigned)monitor_cycle_requests_,(unsigned)monitor_cycle_matched_,(unsigned)monitor_cycle_timeouts_,(unsigned)monitor_cycle_unrelated_,(unsigned)monitor_cycle_unrelated_changed_,(unsigned)monitor_cycle_unrelated_repeated_);
  ESP_LOGI(TAG,"MONITOR CONTROL power=%s%u limit=%s%u room=%s%d outdoor=%s%d F8=%s[%u,%u,%u,%u] E4=%s[%u,%u,%u] E5=%s[%u,%u,%u,%u,%u,%u,%u,%u]",monitor_power_valid_?"":"NA/",(unsigned)monitor_power_,monitor_limit_valid_?"":"NA/",(unsigned)monitor_limit_,monitor_room_valid_?"":"NA/",(int)(int8_t)monitor_room_,monitor_outdoor_valid_?"":"NA/",(int)(int8_t)monitor_outdoor_,monitor_f8_valid_?"":"NA/",(unsigned)monitor_f8_[0],(unsigned)monitor_f8_[1],(unsigned)monitor_f8_[2],(unsigned)monitor_f8_[3],monitor_e4_valid_?"":"NA/",(unsigned)monitor_e4_[0],(unsigned)monitor_e4_[1],(unsigned)monitor_e4_[2],monitor_e5_valid_?"":"NA/",(unsigned)monitor_e5_[0],(unsigned)monitor_e5_[1],(unsigned)monitor_e5_[2],(unsigned)monitor_e5_[3],(unsigned)monitor_e5_[4],(unsigned)monitor_e5_[5],(unsigned)monitor_e5_[6],(unsigned)monitor_e5_[7]);
}
void ToshibaDiagnosticMonitorUart::log_monitor_bytes_(const std::vector<uint8_t>&r,int16_t reg)const{size_t q=(r.size()+CHUNK-1)/CHUNK;for(size_t i=0;i<q;i++){size_t o=i*CHUNK,n=std::min(CHUNK,r.size()-o);ESP_LOGI(TAG,"MONITOR RAW reg=0x%02X chunk=%u/%u offset=%u bytes=[%s]",(unsigned)reg,(unsigned)(i+1),(unsigned)q,(unsigned)o,format_hex_pretty(r.data()+o,n).c_str());}}
void ToshibaDiagnosticMonitorUart::log_monitor_da_(const std::vector<uint8_t>&r){
  size_t o,n;if(!payload(r,0xDA,o,n)||n<130){ESP_LOGI(TAG,"MONITOR DA decode unavailable payload=%u",(unsigned)n);return;}const uint8_t*p=r.data()+o;unsigned day=p[2];if(day<1||day>31)return;uint32_t wh=le32(p+6+(day-1)*4),now=millis();
  if(monitor_da_previous_valid_&&wh>=monitor_da_previous_wh_){uint32_t d=wh-monitor_da_previous_wh_,ms=now-monitor_da_previous_ms_;ESP_LOGI(TAG,"MONITOR DA accumulated=%uWh delta=%uWh elapsed=%.1fs inferred=%.1fW hypothesis=energy",(unsigned)wh,(unsigned)d,ms/1000.0f,ms?d*3600000.0f/ms:0.0f);}else ESP_LOGI(TAG,"MONITOR DA accumulated=%uWh baseline hypothesis=energy",(unsigned)wh);
  monitor_da_previous_valid_=true;monitor_da_previous_wh_=wh;monitor_da_previous_ms_=now;
}
void ToshibaDiagnosticMonitorUart::log_monitor_b7_partial_()const{if(monitor_b7_partial_.empty())return;ESP_LOGI(TAG,"MONITOR B7 partial attempt=%s length=%u bytes=[%s]",monitor_b7_retry_active_?"retry":"first",(unsigned)monitor_b7_partial_.size(),format_hex_pretty(monitor_b7_partial_).c_str());}
} }
