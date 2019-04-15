#include "metricsprovider.hpp"
#include <windows.h>
#include <WinBase.h>
#include "Shlobj.h"
#include <cpr/cpr.h>
#include <curl/curl.h>
#include <string>
#include <regex>
#include <nlohmann/json.hpp>
#include <Lmcons.h>
#include <codecvt>

class curl_wrapper
{
public:
	class response
	{
	public:
		response(std::string d, int sc) : data(std::move(d)), status_code(sc) {}

		std::string data;
		int         status_code;

		nlohmann::json json()
		{
			return nlohmann::json::parse(data);
		}
	};

public:
	curl_wrapper(std::string dns) : m_curl(curl_easy_init())
	{
		assert(m_curl);

		setup_sentry_dns(dns);

		//set_option(CURLOPT_VERBOSE, 1L);
		set_option(CURLOPT_SSL_VERIFYPEER, 0L);
	}

	~curl_wrapper()
	{
		curl_slist_free_all(m_headers);
		curl_easy_cleanup(m_curl);
	}

	void config_secure_header()
	{
		// add security header
		std::string security_header = "X-Sentry-Auth: Sentry sentry_version=5,sentry_client=crow/";
		security_header += std::string("6.8.9") + ",sentry_timestamp=";
		security_header += std::to_string(34546456);
		security_header += ",sentry_key=" + m_public_key;
		security_header += ",sentry_secret=" + m_secret_key;
		set_header(security_header.c_str());
	}

	response post(const nlohmann::json& payload, const bool compress = false)
	{
		config_secure_header();

		set_header("Content-Type: application/json");

		return post(m_store_url, payload.dump(), compress);
	}

private:
	response post(const std::string& url, const std::string& data, const bool compress = false)
	{
		std::string c_data;

		set_option(CURLOPT_POSTFIELDS, data.c_str());
		set_option(CURLOPT_POSTFIELDSIZE, data.size());

		set_option(CURLOPT_URL, url.c_str());
		set_option(CURLOPT_POST, 1);
		set_option(CURLOPT_WRITEFUNCTION, &write_callback);
		set_option(CURLOPT_WRITEDATA, &string_buffer);

		auto res = curl_easy_perform(m_curl);

		if (res != CURLE_OK) {
			std::string error_msg = std::string("curl_easy_perform() failed: ") + curl_easy_strerror(res);
			throw std::runtime_error(error_msg);
		}

		int status_code;
		curl_easy_getinfo(m_curl, CURLINFO_RESPONSE_CODE, &status_code);

		return { std::move(string_buffer), status_code };
	}

	template<typename T>
	CURLcode set_option(CURLoption option, T parameter)
	{
		return curl_easy_setopt(m_curl, option, parameter);
	}

	void set_header(const char* header)
	{
		m_headers = curl_slist_append(m_headers, header);
		set_option(CURLOPT_HTTPHEADER, m_headers);
	}

private:
	static size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata)
	{
		assert(userdata);
		((std::string*)userdata)->append(ptr, size * nmemb);
		return size * nmemb;
	}

	void setup_sentry_dns(const std::string& dsn)
	{
		if (!dsn.empty()) {
			const std::regex dsn_regex("(http[s]?)://([^:]+):([^@]+)@([^/]+)/([0-9]+)");
			std::smatch      pieces_match;

			if (std::regex_match(dsn, pieces_match, dsn_regex) and pieces_match.size() == 6) {
				const auto scheme = pieces_match.str(1);
				m_public_key = pieces_match.str(2);
				m_secret_key = pieces_match.str(3);
				const auto host = pieces_match.str(4);
				const auto project_id = pieces_match.str(5);
				m_store_url = scheme + "://" + host + "/api/" + project_id + "/store/";
			}
			else {
				throw std::invalid_argument("DNS " + dsn + " is invalid");
			}
		}
		else {
			throw std::invalid_argument("DNS is empty");
		}
	}

private:
	CURL* const        m_curl;
	struct curl_slist* m_headers = nullptr;
	std::string        string_buffer;
	std::string        m_public_key;
	std::string        m_secret_key;
	std::string        m_store_url;
};

MetricsProvider::~MetricsProvider()
{
	m_StopPolling = true;
	if (m_PollingThread.joinable())
	{
		m_PollingThread.join();
	}

	if (m_PipeActive)
	{
		CloseHandle(m_Pipe);
	}
}

bool MetricsProvider::Initialize(std::string name)
{
	m_Pipe = CreateNamedPipe(
		name.c_str(), // name of the pipe // "\\\\.\\pipe\\my_pipe"
		PIPE_ACCESS_INBOUND, // 1-way pipe -- receive only
		PIPE_TYPE_BYTE, // send data as a byte stream
		1, // only allow 1 instance of this pipe
		0, // no outbound buffer
		0, // no inbound buffer
		0, // use default wait time
		NULL // use default security attributes
	);

	if (m_Pipe == NULL || m_Pipe == INVALID_HANDLE_VALUE) {
		return false;
	}

	m_PipeActive = true;
	InitializeMetricsFile();

	return true;
}

void MetricsProvider::Shutdown()
{
	m_StopPolling = true;
	if (m_PollingThread.joinable())
	{
		m_PollingThread.join();
	}

	if (m_PipeActive)
	{
		m_PipeActive = false;
		CloseHandle(m_Pipe);
	}

	MetricsFileClose();
}

bool MetricsProvider::ConnectToClient()
{
	BOOL result = ConnectNamedPipe(m_Pipe, NULL);
	if (!result) {
		CloseHandle(m_Pipe); // close the pipe
		return false;
	}

	return true;
}

void MetricsProvider::StartPollingEvent()
{
	m_PollingThread = std::thread([=]()
	{
		while (!m_StopPolling)
		{
			static const int BufferSize = sizeof(MetricsMessage);
			MetricsMessage message;
			DWORD numBytesRead = 0;
			BOOL result = ReadFile(
				m_Pipe,
				&message, // the data from the pipe will be put here
				BufferSize, // number of bytes allocated
				&numBytesRead, // this will store number of bytes actually read
				NULL // not using overlapped IO
			);

			if (!result)
				continue;

			if (message.type == MessageType::Status)
			{
				// Write to the file
				MetricsFileSetStatus(std::string(message.param1));
			}
			else if (message.type == MessageType::Shutdown)
			{
				m_ServerExitedSuccessfully = true;
				m_StopPolling = true;
			}
			else if (message.type == MessageType::Pid)
			{
				m_ServerPid = *reinterpret_cast<DWORD*>(message.param1);
			}
		}
	});
}

bool MetricsProvider::ServerIsActive()
{
	auto IsProcessRunning = [](DWORD pid)
	{
		HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
		DWORD ret = WaitForSingleObject(process, 0);
		CloseHandle(process);
		return ret == WAIT_TIMEOUT;
	};

	// If we can't check by the server pid, better set that it exited
	return m_ServerPid != 0 && IsProcessRunning(m_ServerPid);
}

bool MetricsProvider::ServerExitedSuccessfully()
{
	return m_ServerExitedSuccessfully;
}

void MetricsProvider::InitializeMetricsFile()
{
	HRESULT hResult;
	PWSTR   ppszPath;

	hResult = SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, NULL, &ppszPath);

	std::wstring appdata_path; 

	appdata_path.assign(ppszPath);
	appdata_path.append(L"\\obs-studio-node-server");

	CoTaskMemFree(ppszPath);

	try {
		std::wstring file_path = appdata_path + L"\\" + std::wstring(CrashMetricsFilename);
		auto         metrics_file_read = std::ifstream(file_path);
		if (metrics_file_read.is_open()) {
			std::string metrics_string;
			getline(metrics_file_read, metrics_string);

			// Check if the string is empty, in that case SLOBS crashed before initializing
			if (metrics_string.length() == 0) {
				metrics_string = std::string("generic-idle");
			}

			if (metrics_string != "shutdown") {
				curl_wrapper curl(
					"https://7376a60665cd40bebbd59d6bf8363172:13804c42a5a84504bb5475050f6392e0@sentry.io/1406061");

				nlohmann::json j;
				nlohmann::json tags;
				char        name[UNLEN + 1];
				DWORD          name_len = UNLEN + 1;
				using convert_typeX     = std::codecvt_utf8<wchar_t>;
				std::wstring_convert<convert_typeX, wchar_t> converterX;

				j["message"] = metrics_string;
				j["level"] = "info";

				// User name
				if (GetUserName(name, &name_len) != 0) {
					tags["user.name"] = std::string(name);
				}

				// Computer name
				name_len = UNLEN + 1;
				if (GetComputerName(name, &name_len) != 0) {
					tags["computer.name"] = std::string(name);
				}

				// Version
				tags["version"] = "test";

				j["tags"] = tags;

				curl.post(j, true).data;
			}

			metrics_file_read.close();
		}

		m_MetricsFile = std::ofstream(file_path, std::ios::trunc | std::ios::ate);
	}
	catch (...) {
	}
}

void MetricsProvider::MetricsFileSetStatus(std::string status)
{
	if (m_MetricsFile.is_open()) {
		m_MetricsFile.seekp(0, std::ios::beg);
		m_MetricsFile << status << std::endl;
	}
}

void MetricsProvider::MetricsFileClose()
{
	if (m_MetricsFile.is_open()) {
		MetricsFileSetStatus("shutdown");
		m_MetricsFile.close();
	}
}