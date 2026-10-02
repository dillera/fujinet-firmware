#include "fnConfig.h"
#include <cstring>

void fnConfig::store_gdrive_refresh_token(const std::string &refresh_token)
{
    if (_gdrive.refresh_token == refresh_token)
        return;
    _gdrive.refresh_token = refresh_token;
    _dirty = true;
}

void fnConfig::store_gdrive_access_token(const std::string &access_token)
{
    if (_gdrive.access_token == access_token)
        return;
    _gdrive.access_token = access_token;
    _dirty = true;
}

void fnConfig::store_gdrive_token_expiry(long expiry)
{
    if (_gdrive.token_expiry == expiry)
        return;
    _gdrive.token_expiry = expiry;
    _dirty = true;
}

// A new client cannot refresh the old one's grant, so it is dropped.
void fnConfig::store_gdrive_client(const std::string &client_id, const std::string &client_secret)
{
    std::string id = client_id.empty() ? GOOGLE_DEFAULT_CLIENT_ID : client_id;
    if (_gdrive.client_id == id && _gdrive.client_secret == client_secret)
        return;
    _gdrive.client_id = id;
    _gdrive.client_secret = client_secret;
    _gdrive.refresh_token.clear();
    _gdrive.access_token.clear();
    _gdrive.token_expiry = 0;
    _dirty = true;
}

void fnConfig::_read_section_gdrive(std::stringstream &ss)
{
    std::string line;

    while (_read_line(ss, line, '[') >= 0)
    {
        std::string name;
        std::string value;
        if (_split_name_value(line, name, value))
        {
            if (strcasecmp(name.c_str(), "refresh_token") == 0)
                _gdrive.refresh_token = value;
            else if (strcasecmp(name.c_str(), "access_token") == 0)
                _gdrive.access_token = value;
            else if (strcasecmp(name.c_str(), "token_expiry") == 0)
                _gdrive.token_expiry = atol(value.c_str());
            else if (strcasecmp(name.c_str(), "client_id") == 0 && !value.empty())
                _gdrive.client_id = value;
            else if (strcasecmp(name.c_str(), "client_secret") == 0)
                _gdrive.client_secret = value;
            else if (strcasecmp(name.c_str(), "relay") == 0 && !value.empty())
            {
                _gdrive.relay = value;
                while (!_gdrive.relay.empty() && _gdrive.relay.back() == '/')
                    _gdrive.relay.pop_back();
            }
        }
    }
}
