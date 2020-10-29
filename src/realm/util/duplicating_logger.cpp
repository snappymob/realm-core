#include <realm/util/duplicating_logger.hpp>

using namespace realm;
using util::DuplicatingLogger;


void DuplicatingLogger::do_log(Logger::Level level, std::string const& message)
{
    Logger::do_log(m_base_logger, level, message); // Throws
    Logger::do_log(m_aux_logger, level, message);  // Throws
}
