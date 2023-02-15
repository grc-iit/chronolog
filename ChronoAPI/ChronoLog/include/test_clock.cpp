#include "ClockSync.h"
#include <iostream>

int main()
{

  ClockSynchronization<ClocksourceCStyle> *a = new ClockSynchronization<ClocksourceCStyle> ();

  uint64_t t = a->Timestamp();
  uint64_t ndays = t/(uint64_t)(24*60*60);
  uint64_t nyears = ndays/(uint64_t)365;
  std::cout <<" t = "<<a->Timestamp()<<std::endl;
  std::cout <<" ndays = "<<ndays<<std::endl;
  std::cout <<" nyears = "<<nyears<<std::endl;

  ClockSynchronization<ClocksourceCPPStyle> *b = new ClockSynchronization<ClocksourceCPPStyle> ();

  t = b->Timestamp();
  ndays = t/(uint64_t)(24*60*60);
  std::cout <<" t = "<<t<<std::endl;

  delete a;
  delete b;
}
