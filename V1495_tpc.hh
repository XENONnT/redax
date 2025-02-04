#ifndef _V1495_TPC_HH_
#define _V1495_TPC_HH_

#include "V1495.hh"

class V1495_TPC : public V1495 {
  public:
    V1495_TPC(std::shared_ptr<MongoLog>&, std::shared_ptr<Options>&, int, int, unsigned);
    virtual ~V1495_TPC();
    virtual int Arm(std::map<std::string, int>&);
    virtual int ArmSoft(std::map<std::string, int>&);
    virtual int BeforeSINStart();
    virtual int BeforeSINStop();

  protected:
    const uint32_t fModeReg;
    const uint32_t fFracLtOnAReg;
    const uint32_t fFracLtOnBReg;
    const uint32_t fFracLtOffAReg;
    const uint32_t fFracLtOffBReg;
    const uint32_t fAntiVetoDelayAReg;
    const uint32_t fAntiVetoDelayBReg;
    const uint32_t fAntiVetoDurationAReg;
    const uint32_t fAntiVetoDurationBReg;

    uint32_t fMode;
    uint32_t fFracLTVetoOn_clk, fFracLTVetoOff_clk;
    uint32_t fAntiVetoDelay_clk, fAntiVetoDuration_clk;
};

#endif // _V1495_TPC_HH_ defined
