#include "online_config.h"

namespace aml::online {

OnlineConfig& config() {
    static OnlineConfig instance;
    return instance;
}

}  // namespace aml::online
