/* 
 * File:   chakra_engine.hpp
 * Author: alex
 *
 * Created on January 30, 2018, 2:11 PM
 */

#ifndef WILTON_CHAKRA_ENGINE_HPP
#define WILTON_CHAKRA_ENGINE_HPP

#include <string>

#include "staticlib/json.hpp"
#include "staticlib/pimpl.hpp"

#include "wilton/support/buffer.hpp"
#include "wilton/support/exception.hpp"
#include "wilton/support/script_engine_map.hpp"

namespace wilton {
namespace chakra {

class chakra_engine : public sl::pimpl::object {
protected:
    /**
     * implementation class
     */
    class impl;
public:
    /**
     * PIMPL-specific constructor
     * 
     * @param pimpl impl object
     */
    PIMPL_CONSTRUCTOR(chakra_engine)

    chakra_engine(sl::io::span<const char> init_code);
    
    support::buffer run_callback_script(sl::io::span<const char> callback_script_json);
};

} // namespace
}

#endif /* WILTON_CHAKRA_ENGINE_HPP */

