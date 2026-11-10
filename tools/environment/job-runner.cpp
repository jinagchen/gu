#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Handle {
    HANDLE value;
    explicit Handle(HANDLE handle) : value(handle) {}
    ~Handle() {
        if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

void Require(bool ok, const char* operation) {
    if (!ok) throw std::runtime_error(std::string(operation) + ": " +
                                     std::to_string(GetLastError()));
}

std::wstring Quote(const std::wstring& argument) {
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (wchar_t character : argument) {
        if (character == L'\\') {
            ++slashes;
        } else {
            result.append(slashes * (character == L'"' ? 2 : 1), L'\\');
            if (character == L'"') result += L'\\';
            result += character;
            slashes = 0;
        }
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}

int Child(const std::wstring& mode, const wchar_t* executable) {
    if (mode == L"reject") {
        void* memory = VirtualAlloc(nullptr, SIZE_T{96} << 20,
                                   MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        const DWORD error = GetLastError();
        if (memory) VirtualFree(memory, 0, MEM_RELEASE);
        Require(!memory && error == ERROR_COMMITMENT_LIMIT,
                "Expected job commit rejection");
        std::puts("over_limit_commit_rejected=pass");
        return 0;
    }
    void* memory = VirtualAlloc(nullptr, SIZE_T{16} << 20,
                               MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    Require(memory != nullptr, "Child VirtualAlloc");
    auto* pages = static_cast<volatile unsigned char*>(memory);
    for (std::size_t i = 0; i < (SIZE_T{16} << 20); i += 4096) pages[i] = 1;
    if (mode == L"tree") {
        std::wstring command = Quote(executable) + L" --child leaf";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        Require(CreateProcessW(executable, command.data(), nullptr, nullptr,
                               FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                               &startup, &process) != 0, "Create child");
        Handle child(process.hProcess);
        Handle thread(process.hThread);
        Require(WaitForSingleObject(child.value, 5000) == WAIT_OBJECT_0,
                "Wait child");
        DWORD code = 1;
        Require(GetExitCodeProcess(child.value, &code) != 0 && code == 0,
                "Child status");
    } else {
        Sleep(150);
    }
    VirtualFree(memory, 0, MEM_RELEASE);
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 3 && std::wstring(argv[1]) == L"--child")
            return Child(argv[2], argv[0]);
        Require(argc >= 5, "Usage: job_runner MiB timeout_seconds metrics.json exe [args]");
        const auto limit_mib = std::stoull(argv[1]);
        const auto seconds = std::stoull(argv[2]);
        Require(limit_mib > 0 && limit_mib <= 65536 &&
                    seconds > 0 && seconds <= 3600, "Invalid resource limits");
        std::ofstream output{std::filesystem::path(argv[3])};
        Require(output.good(), "Open metrics file");
        Handle job(CreateJobObjectW(nullptr, nullptr));
        Require(job.value != nullptr, "CreateJobObject");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_JOB_MEMORY | JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
            JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
        limits.JobMemoryLimit = static_cast<SIZE_T>(limit_mib << 20);
        Require(SetInformationJobObject(job.value, JobObjectExtendedLimitInformation,
                    &limits, sizeof(limits)) != 0, "Set job limits");
        std::wstring command;
        for (int i = 4; i < argc; ++i) {
            if (i != 4) command += L' ';
            command += Quote(argv[i]);
        }
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        HANDLE input_copy = nullptr, output_copy = nullptr, error_copy = nullptr;
        const HANDLE current = GetCurrentProcess();
        Require(DuplicateHandle(current, GetStdHandle(STD_INPUT_HANDLE), current,
                    &input_copy, 0, TRUE, DUPLICATE_SAME_ACCESS) != 0,
                "Duplicate stdin");
        Handle input_handle(input_copy);
        Require(DuplicateHandle(current, GetStdHandle(STD_OUTPUT_HANDLE), current,
                    &output_copy, 0, TRUE, DUPLICATE_SAME_ACCESS) != 0,
                "Duplicate stdout");
        Handle output_handle(output_copy);
        Require(DuplicateHandle(current, GetStdHandle(STD_ERROR_HANDLE), current,
                    &error_copy, 0, TRUE, DUPLICATE_SAME_ACCESS) != 0,
                "Duplicate stderr");
        Handle error_handle(error_copy);
        startup.hStdInput = input_copy;
        startup.hStdOutput = output_copy;
        startup.hStdError = error_copy;
        PROCESS_INFORMATION process{};
        Require(CreateProcessW(argv[4], command.data(), nullptr, nullptr, TRUE,
                    CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr,
                    &startup, &process) != 0, "CreateProcess suspended");
        Handle child(process.hProcess);
        Handle thread(process.hThread);
        if (!AssignProcessToJobObject(job.value, child.value)) {
            TerminateProcess(child.value, 1);
            WaitForSingleObject(child.value, 5000);
            throw std::runtime_error("AssignProcessToJobObject failed");
        }
        Require(ResumeThread(thread.value) != static_cast<DWORD>(-1),
                "ResumeThread");
        const ULONGLONG start = GetTickCount64();
        std::uint64_t sampled_private = 0, sampled_working_set = 0;
        bool timed_out = false;
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
        // A fixed, ample PID buffer keeps measurement allocation bounded.
        std::vector<ULONG_PTR> pid_storage(1026);
        while (true) {
            Require(QueryInformationJobObject(job.value,
                        JobObjectBasicAccountingInformation, &accounting,
                        sizeof(accounting), nullptr) != 0, "Query job accounting");
            auto* list = reinterpret_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST*>(
                pid_storage.data());
            Require(QueryInformationJobObject(job.value, JobObjectBasicProcessIdList,
                        list, static_cast<DWORD>(pid_storage.size() * sizeof(ULONG_PTR)),
                        nullptr) != 0, "Query job PIDs");
            std::uint64_t private_total = 0, ws_total = 0;
            for (DWORD i = 0; i < list->NumberOfProcessIdsInList; ++i) {
                Handle member(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                    FALSE, static_cast<DWORD>(list->ProcessIdList[i])));
                if (!member.value) continue;  // May have exited after PID snapshot.
                PROCESS_MEMORY_COUNTERS_EX memory{};
                memory.cb = sizeof(memory);
                if (GetProcessMemoryInfo(member.value,
                        reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
                        sizeof(memory))) {
                    private_total += memory.PrivateUsage;
                    ws_total += memory.WorkingSetSize;
                }
            }
            sampled_private = (std::max)(sampled_private, private_total);
            sampled_working_set = (std::max)(sampled_working_set, ws_total);
            if (accounting.ActiveProcesses == 0) break;
            if (GetTickCount64() - start > seconds * 1000) {
                timed_out = true;
                Require(TerminateJobObject(job.value, 124) != 0, "Terminate timed out job");
                Require(WaitForSingleObject(child.value, 5000) == WAIT_OBJECT_0,
                        "Wait timed out process");
                break;
            }
            Sleep(5);
        }
        DWORD exit_code = 1;
        Require(GetExitCodeProcess(child.value, &exit_code) != 0, "Get child status");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION measured{};
        Require(QueryInformationJobObject(job.value, JobObjectExtendedLimitInformation,
                    &measured, sizeof(measured), nullptr) != 0, "Query job peak");
        output << "{\n  \"schema_version\": 1,\n"
               << "  \"limit_commit_bytes\": " << limits.JobMemoryLimit << ",\n"
               << "  \"kernel_peak_job_commit_bytes\": " << measured.PeakJobMemoryUsed << ",\n"
               << "  \"sampled_peak_private_bytes\": " << sampled_private << ",\n"
               << "  \"sampled_peak_working_set_bytes\": " << sampled_working_set << ",\n"
               << "  \"total_processes\": " << accounting.TotalProcesses << ",\n"
               << "  \"sample_interval_ms\": 5,\n"
               << "  \"elapsed_ms\": " << GetTickCount64() - start << ",\n"
               << "  \"timed_out\": " << (timed_out ? "true" : "false") << ",\n"
               << "  \"exit_code\": " << exit_code << "\n}\n";
        output.flush();
        Require(output.good(), "Write metrics");
        return timed_out ? 124 : exit_code == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "job_runner_failed: %s\n", error.what());
        return 1;
    }
}
