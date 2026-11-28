#include "agent_io/engine.hpp"

#include <boost/json.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {

namespace json = boost::json;
using namespace agent_io;

const char* ErrorName(ErrorCode code) {
    switch (code) {
    case ErrorCode::invalid_argument: return "invalid_argument";
    case ErrorCode::not_found: return "not_found";
    case ErrorCode::already_exists: return "already_exists";
    case ErrorCode::resource_exhausted: return "resource_exhausted";
    case ErrorCode::deadline_exceeded: return "deadline_exceeded";
    case ErrorCode::search_incomplete: return "search_incomplete";
    case ErrorCode::io_error: return "io_error";
    case ErrorCode::corruption: return "corruption";
    case ErrorCode::unsupported: return "unsupported";
    case ErrorCode::closed: return "closed";
    case ErrorCode::recovery_required: return "recovery_required";
    }
    return "unknown";
}

json::object ErrorReply(const Error& error) {
    return {{"status", "error"},
            {"code", ErrorName(error.code)},
            {"message", error.message},
            {"system_error", error.system_error},
            {"write_effect", error.write_effect == WriteEffect::may_have_applied ?
                "may_have_applied" : "none"},
            {"attempted_sequence", error.attempted_sequence ?
                json::value(std::to_string(*error.attempted_sequence)) : json::value(nullptr)}};
}

json::object Invoke(Engine& engine, const json::object& request) {
    const auto& operation = request.at("operation").as_string();
    const auto& vector = request.at("vector").as_array();
    std::vector<float> values;
    values.reserve(vector.size());
    for (const auto& element : vector) {
        const auto number = element.to_number<double>();
        if (!std::isfinite(number) ||
            std::abs(number) > (std::numeric_limits<float>::max)())
            throw std::invalid_argument("vector must contain finite float32 values");
        values.push_back(static_cast<float>(number));
    }

    if (operation == "search") {
        SearchOptions options;
        if (const auto* k = request.if_contains("k"))
            options.k = k->to_number<std::uint32_t>();
        options.candidate_limit = (std::max)(options.k, options.candidate_limit);
        const auto answer = engine.Search(values, options);
        if (const auto* error = std::get_if<Error>(&answer)) return ErrorReply(*error);
        const auto& result = std::get<SearchResult>(answer);
        json::array neighbors;
        for (const auto& row : result.neighbors) {
            neighbors.emplace_back(json::object{
                {"id", std::to_string(row.id)}, {"version", std::to_string(row.version)},
                {"squared_distance", row.squared_distance}});
        }
        return {{"status", "ok"}, {"snapshot_sequence", std::to_string(result.snapshot_sequence)},
                {"neighbors", std::move(neighbors)}};
    }
    if (operation != "insert" && operation != "upsert")
        throw std::invalid_argument("operation must be search, insert or upsert");
    const auto& id_text = request.at("id").as_string();
    VectorId id = 0;
    const auto parsed = std::from_chars(id_text.data(), id_text.data() + id_text.size(), id);
    if (parsed.ec != std::errc{} || parsed.ptr != id_text.data() + id_text.size() || id == 0)
        throw std::invalid_argument("id must be a positive decimal uint64 string");
    const auto written = operation == "insert" ? engine.Insert(id, values, {}) :
                                                  engine.Upsert(id, values, {});
    if (const auto* error = std::get_if<Error>(&written)) return ErrorReply(*error);
    const auto& receipt = std::get<WriteReceipt>(written);
    return {{"status", "ok"}, {"id", std::to_string(receipt.id)},
            {"sequence", std::to_string(receipt.sequence)}};
}

json::object Run(const std::filesystem::path& directory) {
    constexpr std::size_t input_limit = 4u << 20;
    std::string line;
    for (char byte = 0; std::cin.get(byte) && byte != '\n';) {
        if (line.size() == input_limit)
            throw std::invalid_argument("request exceeds the 4 MiB input limit");
        line.push_back(byte);
    }
    const auto request = json::parse(line);
    constexpr std::uint64_t mib = 1u << 20;
    EngineOptions options;
    options.memory = MemoryBudget{96 * mib, 8 * mib, 8 * mib, 8 * mib, 64 * mib, 8 * mib};
    options.max_queries = 1;
    options.direct_reads = false;
    auto opened = Engine::Open(directory, options);
    if (const auto* error = std::get_if<Error>(&opened)) return ErrorReply(*error);
    return Invoke(*std::get<std::unique_ptr<Engine>>(opened), request.as_object());
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        std::cerr << "usage: vector_tool <index-directory>\n";
        return 2;
    }
    json::object response;
    try {
        response = Run(argv[1]);
    } catch (const std::invalid_argument& error) {
        response = ErrorReply({ErrorCode::invalid_argument, error.what()});
    } catch (const boost::system::system_error& error) {
        response = ErrorReply({ErrorCode::invalid_argument, error.what()});
    }
    std::cout << json::serialize(response) << '\n';
    return std::cout ? 0 : 1;
}
