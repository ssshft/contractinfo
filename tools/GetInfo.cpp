#include <iostream>
#include "data_struct.h"
#include "securitymanager.h"

int main() {
    sm::SecurityManager* smc = new sm::SecurityManager("localhost", 9379, "refZa2KcTj$QjyXk", true);
    md::InstrumentInfo info;
    smc->get_instrument_info(BINANCE, SPOT, "BTC-USDT", info);
    std::cout << info.getString() << std::endl;

    std::vector<md::InstrumentInfo> instInfoVec;
    smc->get_all_instruments(instInfoVec);

    for (size_t i = 0; i < instInfoVec.size(); ++i) {
        std::cout << instInfoVec[i].getString() << std::endl;
    }
    return 0;
}