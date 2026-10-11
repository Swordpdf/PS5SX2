// Orbis net stubs: pcap capture backend absent (no libpcap). DEV9 sockets
// backend works; pcap selection will fail gracefully at runtime.
#include "DEV9/pcap_io.h"

PCAPAdapter::PCAPAdapter()
  : switched(false)
  , blocking(false)
{
}
PCAPAdapter::~PCAPAdapter()
{
}
bool PCAPAdapter::blocks()
{
  return false;
}
bool PCAPAdapter::isInitialised()
{
  return false;
}
bool PCAPAdapter::recv(NetPacket* pkt)
{
  (void)pkt;
  return false;
}
bool PCAPAdapter::send(NetPacket* pkt)
{
  (void)pkt;
  return false;
}
void PCAPAdapter::reloadSettings()
{
}
