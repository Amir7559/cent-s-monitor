
#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <regex>
#include <cstdlib>
#include <algorithm>
#include <fstream>
#include <ctime>
#include <cctype>
#include <curl/curl.h>

using namespace std;

const string CISIA_URL = "https://testcisia.it/calendario.php?lingua=inglese&tolc=cents";
const string CHAT_ID = "7332232667";
const string STATE_FILE = "availability_state.txt";
const string STATUS_FILE = "monitor_status.txt";
const string OFFSET_FILE = "telegram_update_id.txt";

map<string, string> previousStatus;
int lastTotalEntries = 0;
int lastAvailable = 0;
bool lastScanOK = false;
string lastCheckTime = "Not checked yet";

string currentTime() {
    time_t now = time(nullptr);
    tm* t = gmtime(&now);
    if (!t) return "Unknown";
    char buffer[80];
    strftime(buffer, sizeof(buffer), "%d/%m/%Y %H:%M:%S UTC", t);
    return buffer;
}

size_t writeCallback(void* data, size_t size, size_t count, void* userp) {
    size_t bytes = size * count;
    static_cast<string*>(userp)->append(static_cast<char*>(data), bytes);
    return bytes;
}

string upperCase(string value) {
    transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(toupper(c)); });
    return value;
}

string cleanHTML(string html) {
    html = regex_replace(html, regex("<[^>]*>"), " ");
    html = regex_replace(html, regex("&nbsp;|&#160;"), " ");
    html = regex_replace(html, regex("&amp;"), "&");
    html = regex_replace(html, regex("&quot;"), "\"");
    html = regex_replace(html, regex("&#39;"), "'");
    html = regex_replace(html, regex("\\s+"), " ");
    size_t a = html.find_first_not_of(" \t\r\n");
    if (a == string::npos) return "";
    size_t b = html.find_last_not_of(" \t\r\n");
    return html.substr(a, b - a + 1);
}

bool httpGet(CURL* curl, const string& url, string& response, long timeout = 30) {
    response.clear();
    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 CEnT-S-Monitor/Cloud");

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        cerr << "HTTP error: " << curl_easy_strerror(rc) << endl;
        return false;
    }

    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    return code == 200;
}

bool sendTelegram(CURL* curl, const string& token, const string& message) {
    curl_easy_reset(curl);
    string url = "https://api.telegram.org/bot" + token + "/sendMessage";

    char* encoded = curl_easy_escape(curl, message.c_str(), static_cast<int>(message.size()));
    if (!encoded) return false;

    string body = "chat_id=" + CHAT_ID + "&text=" + encoded;
    curl_free(encoded);

    string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 25L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    if (rc == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);

    if (rc != CURLE_OK) {
        cerr << "Telegram error: " << curl_easy_strerror(rc) << endl;
        return false;
    }

    cout << "Telegram response: " << response << endl;
    return code == 200 && response.find("\"ok\":true") != string::npos;
}

void loadState() {
    ifstream in(STATE_FILE);
    string line;

    while (getline(in, line)) {
        size_t p = line.rfind('|');
        if (p == string::npos) continue;

        string key = line.substr(0, p);
        string value = line.substr(p + 1);

        if (value == "AVAILABLE" || value == "FULL")
            previousStatus[key] = value;
    }

    cout << "Previous availability records: " << previousStatus.size() << endl;
}

bool saveState() {
    ofstream out(STATE_FILE, ios::trunc);
    if (!out) return false;

    for (const auto& item : previousStatus)
        out << item.first << "|" << item.second << "\n";

    return static_cast<bool>(out);
}

void loadMonitorStatus() {
    ifstream in(STATUS_FILE);
    string line;

    while (getline(in, line)) {
        size_t p = line.find('=');
        if (p == string::npos) continue;

        string key = line.substr(0, p);
        string value = line.substr(p + 1);

        if (key == "lastScanOK") lastScanOK = (value == "1");
        else if (key == "lastTotalEntries") {
            try { lastTotalEntries = stoi(value); } catch (...) {}
        }
        else if (key == "lastAvailable") {
            try { lastAvailable = stoi(value); } catch (...) {}
        }
        else if (key == "lastCheckTime") lastCheckTime = value;
    }
}

bool saveMonitorStatus() {
    ofstream out(STATUS_FILE, ios::trunc);
    if (!out) return false;

    out << "lastScanOK=" << (lastScanOK ? 1 : 0) << "\n";
    out << "lastTotalEntries=" << lastTotalEntries << "\n";
    out << "lastAvailable=" << lastAvailable << "\n";
    out << "lastCheckTime=" << lastCheckTime << "\n";

    return static_cast<bool>(out);
}

bool notifyAvailability(CURL* curl, const string& token, const string& university,
                        const string& city, const string& date) {
    string msg = "NEW CEnT@CASA AVAILABILITY!\n\nUniversity: " + university +
        "\nCity: " + city + "\nTest date: " + date +
        "\n\nBook your seat on CISIA:\n" + CISIA_URL;

    cout << "NEW AVAILABILITY: " << university << " / " << date << endl;
    return sendTelegram(curl, token, msg);
}

bool notifyFull(CURL* curl, const string& token, const string& university,
                const string& city, const string& date) {
    string msg = "CEnT@CASA CAPACITY UPDATE\n\nUniversity: " + university +
        "\nCity: " + city + "\nTest date: " + date +
        "\n\nStatus: POSTI ESAURITI\nPreviously available seats are now full.";

    return sendTelegram(curl, token, msg);
}

bool checkAvailability(CURL* telegramCurl, const string& html, const string& token) {
    const regex rowPattern(R"REGEX(<tr\b[^>]*>(.*?)</tr>)REGEX", regex_constants::icase);
    const regex cellPattern(R"REGEX(<td\b[^>]*>(.*?)</td>)REGEX", regex_constants::icase);

    int found = 0, availableCount = 0;

    for (auto row = sregex_iterator(html.begin(), html.end(), rowPattern);
         row != sregex_iterator(); ++row) {

        string rowHTML = (*row)[1].str();
        vector<string> cells;

        for (auto cell = sregex_iterator(rowHTML.begin(), rowHTML.end(), cellPattern);
             cell != sregex_iterator(); ++cell) {
            cells.push_back(cleanHTML((*cell)[1].str()));
        }

        if (cells.size() < 8 || upperCase(cells[0]) != "CENT@CASA")
            continue;

        string university = cells[1];
        string city = cells[3];
        string status = upperCase(cells[6]);
        string date = cells[7];

        bool available = status.find("POSTI DISPONIBILI") != string::npos;
        bool full = status.find("POSTI ESAURITI") != string::npos;

        if (!available && !full) continue;

        ++found;
        if (available) ++availableCount;

        string key = university + "|" + date;

        cout << university << " | " << city << " | " << date << " | " << status << endl;

        auto it = previousStatus.find(key);

        if (it == previousStatus.end()) {
            if (available && notifyAvailability(telegramCurl, token, university, city, date))
                previousStatus[key] = "AVAILABLE";
            else if (full)
                previousStatus[key] = "FULL";
        }
        else if (it->second == "FULL" && available) {
            if (notifyAvailability(telegramCurl, token, university, city, date))
                it->second = "AVAILABLE";
            else
                cout << "Opening alert failed; it will be retried." << endl;
        }
        else if (it->second == "AVAILABLE" && full) {
            if (notifyFull(telegramCurl, token, university, city, date))
                it->second = "FULL";
            else
                cout << "Full alert failed; it will be retried." << endl;
        }
    }

    if (found == 0) {
        lastScanOK = false;
        cerr << "No CEnT@CASA rows found; previous availability state retained." << endl;
        return false;
    }

    lastScanOK = true;
    lastTotalEntries = found;
    lastAvailable = availableCount;
    lastCheckTime = currentTime();

    cout << "Total CEnT@CASA entries: " << found
         << "\nAvailable entries: " << availableCount << endl;

    if (!saveState())
        cerr << "WARNING: Could not save availability state." << endl;

    return true;
}

vector<string> extractUpdateObjects(const string& json) {
    vector<string> result;
    size_t p = json.find("\"result\"");
    if (p == string::npos) return result;

    size_t startArray = json.find('[', p);
    if (startArray == string::npos) return result;

    int depth = 0;
    bool inString = false, escaped = false;
    size_t objectStart = string::npos;

    for (size_t i = startArray + 1; i < json.size(); ++i) {
        char c = json[i];

        if (inString) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') inString = false;
            continue;
        }

        if (c == '"') inString = true;
        else if (c == '{') {
            if (depth == 0) objectStart = i;
            ++depth;
        }
        else if (c == '}') {
            --depth;
            if (depth == 0 && objectStart != string::npos) {
                result.push_back(json.substr(objectStart, i - objectStart + 1));
                objectStart = string::npos;
            }
        }
        else if (c == ']' && depth == 0) break;
    }

    return result;
}

bool getJsonObject(const string& json, const string& field, string& output) {
    size_t p = json.find("\"" + field + "\"");
    if (p == string::npos) return false;

    p = json.find('{', p);
    if (p == string::npos) return false;

    size_t start = p;
    int depth = 0;
    bool inString = false, escaped = false;

    for (; p < json.size(); ++p) {
        char c = json[p];

        if (inString) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') inString = false;
            continue;
        }

        if (c == '"') inString = true;
        else if (c == '{') ++depth;
        else if (c == '}' && --depth == 0) {
            output = json.substr(start, p - start + 1);
            return true;
        }
    }

    return false;
}

bool getJsonNumber(const string& json, const string& field, long long& value) {
    smatch m;

    if (!regex_search(json, m,
        regex("\\\"" + field + "\\\"\\s*:\\s*(-?[0-9]+)")))
        return false;

    try {
        value = stoll(m[1].str());
        return true;
    }
    catch (...) {
        return false;
    }
}

bool getJsonString(const string& json, const string& field, string& value) {
    string key = "\"" + field + "\"";
    size_t p = json.find(key);
    if (p == string::npos) return false;

    p = json.find(':', p + key.size());
    if (p == string::npos) return false;

    ++p;
    while (p < json.size() && isspace(static_cast<unsigned char>(json[p]))) ++p;

    if (p >= json.size() || json[p] != '"') return false;

    ++p;
    value.clear();
    bool escaped = false;

    for (; p < json.size(); ++p) {
        char c = json[p];

        if (escaped) {
            switch (c) {
                case 'n': value += '\n'; break;
                case 'r': value += '\r'; break;
                case 't': value += '\t'; break;
                case '"': value += '"'; break;
                case '\\': value += '\\'; break;
                case '/': value += '/'; break;
                default: value += c;
            }
            escaped = false;
        }
        else if (c == '\\') escaped = true;
        else if (c == '"') return true;
        else value += c;
    }

    return false;
}

long long loadOffset() {
    ifstream in(OFFSET_FILE);
    long long offset = 0;
    if (in) in >> offset;
    return offset < 0 ? 0 : offset;
}

void saveOffset(long long offset) {
    ofstream out(OFFSET_FILE, ios::trunc);
    if (out) out << offset;
}

void initializeOffset(CURL* curl, const string& token, long long& offset) {
    ifstream existing(OFFSET_FILE);

    if (existing.good()) {
        offset = loadOffset();
        return;
    }

    string response;

    if (!httpGet(curl,
        "https://api.telegram.org/bot" + token + "/getUpdates?timeout=0",
        response, 15)) {
        offset = 0;
        return;
    }

    long long highest = -1;

    for (const string& item : extractUpdateObjects(response)) {
        long long id = -1;
        if (getJsonNumber(item, "update_id", id) && id > highest)
            highest = id;
    }

    offset = highest >= 0 ? highest + 1 : 0;
    saveOffset(offset);
}

void processCommand(CURL* sendCurl, const string& token, const string& command) {
    string cmd = command.substr(0, command.find(' '));
    size_t at = cmd.find('@');
    if (at != string::npos) cmd = cmd.substr(0, at);

    if (cmd != "/status") return;

    string message = "CEnT@CASA MONITOR STATUS\n\n";
    message += "Last calendar check: " + lastCheckTime + "\n";
    message += string("Calendar scan: ") +
        (lastScanOK ? "SUCCESS\n" : "FAILED / NOT VERIFIED\n");
    message += "Total CEnT@CASA entries: " + to_string(lastTotalEntries) + "\n";
    message += "Available entries: " + to_string(lastAvailable) + "\n";
    message += "Scheduled calendar check: every 5 minutes\n";
    message += "Commands are checked on each scheduled run (response may take up to about 5 minutes).";

    sendTelegram(sendCurl, token, message);
}

void pollTelegram(CURL* pollCurl, CURL* sendCurl,
                  const string& token, long long& offset) {
    string response;
    string url = "https://api.telegram.org/bot" + token +
        "/getUpdates?offset=" + to_string(offset) + "&timeout=0";

    if (!httpGet(pollCurl, url, response, 20)) {
        cerr << "Telegram polling failed." << endl;
        return;
    }

    for (const string& update : extractUpdateObjects(response)) {
        long long id = -1, chatId = -1;
        string messageObject, chatObject, text;

        if (!getJsonNumber(update, "update_id", id)) continue;

        if (getJsonObject(update, "message", messageObject) &&
            getJsonObject(messageObject, "chat", chatObject) &&
            getJsonNumber(chatObject, "id", chatId) &&
            getJsonString(messageObject, "text", text) &&
            to_string(chatId) == CHAT_ID) {

            cout << "Telegram command received: " << text << endl;
            processCommand(sendCurl, token, text);
        }

        offset = id + 1;
        saveOffset(offset);
    }
}

int main() {
    const char* envToken = getenv("BOT_TOKEN");

    if (!envToken || !*envToken) {
        cerr << "BOT_TOKEN secret is missing." << endl;
        return 1;
    }

    string token(envToken);
    curl_global_init(CURL_GLOBAL_DEFAULT);

    CURL* cisiaCurl = curl_easy_init();
    CURL* sendCurl = curl_easy_init();
    CURL* pollCurl = curl_easy_init();

    if (!cisiaCurl || !sendCurl || !pollCurl) {
        cerr << "Could not initialize CURL." << endl;
        if (cisiaCurl) curl_easy_cleanup(cisiaCurl);
        if (sendCurl) curl_easy_cleanup(sendCurl);
        if (pollCurl) curl_easy_cleanup(pollCurl);
        curl_global_cleanup();
        return 1;
    }

    cout << "=== CEnT@CASA CLOUD MONITOR ===" << endl;

    loadState();
    loadMonitorStatus();

    string html;
    cout << "Downloading CISIA calendar..." << endl;

    if (httpGet(cisiaCurl, CISIA_URL, html, 60)) {
        if (!checkAvailability(sendCurl, html, token))
            cout << "CISIA scan did not verify entries." << endl;
    }
    else {
        lastScanOK = false;
        lastCheckTime = currentTime();
        cerr << "CISIA calendar download failed; previous state retained." << endl;
    }

    if (!saveMonitorStatus())
        cerr << "Could not save monitor status." << endl;

    long long offset = 0;
    initializeOffset(pollCurl, token, offset);

    cout << "Checking Telegram for /status..." << endl;
    pollTelegram(pollCurl, sendCurl, token, offset);

    curl_easy_cleanup(cisiaCurl);
    curl_easy_cleanup(sendCurl);
    curl_easy_cleanup(pollCurl);
    curl_global_cleanup();

    cout << "Cloud run completed." << endl;
    return 0;
}
