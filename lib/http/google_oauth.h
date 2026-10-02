/**
 * Where the shared Google grant is made and refreshed.
 *
 * By default a relay holds the OAuth client's secret ([GoogleDrive] relay,
 * see tools/gdrive-relay): Google redirects to it, and refreshes go through
 * it. With [GoogleDrive] client_secret set - a "Desktop app" client in the
 * user's own Google Cloud project - there is no relay: Google redirects the
 * browser to a loopback address, the user pastes that address into the web
 * UI, and FujiNet trades the code and refreshes with Google itself. Google
 * does not treat a Desktop client's secret as confidential.
 */

#ifndef GOOGLE_OAUTH_H
#define GOOGLE_OAUTH_H

#include "../config/fnConfig.h"

#include <cctype>
#include <cstdio>
#include <string>

#define GOOGLE_TOKEN_URL "https://oauth2.googleapis.com/token"
#define GOOGLE_LOOPBACK_REDIRECT "http://127.0.0.1"

inline std::string google_form_encode(const std::string &s)
{
    std::string out;
    for (unsigned char c : s)
    {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out += static_cast<char>(c);
        else
        {
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

// True when FujiNet talks to Google itself rather than through a relay.
inline bool google_direct()
{
    return !Config.get_gdrive_client_secret().empty();
}

inline std::string google_redirect_uri()
{
    return google_direct() ? GOOGLE_LOOPBACK_REDIRECT : Config.get_gdrive_relay() + "/gdrive-callback";
}

inline std::string google_refresh_url()
{
    return google_direct() ? GOOGLE_TOKEN_URL : Config.get_gdrive_relay() + "/gdrive-refresh";
}

inline std::string google_refresh_body(const std::string &refresh_token)
{
    std::string body = "refresh_token=" + google_form_encode(refresh_token);
    if (google_direct())
        body += "&grant_type=refresh_token&client_id=" +
                google_form_encode(Config.get_gdrive_client_id()) +
                "&client_secret=" + google_form_encode(Config.get_gdrive_client_secret());
    return body;
}

// Body that trades an authorization code for tokens, in direct mode.
inline std::string google_code_body(const std::string &code)
{
    return "grant_type=authorization_code&code=" + google_form_encode(code) +
           "&redirect_uri=" + google_form_encode(GOOGLE_LOOPBACK_REDIRECT) +
           "&client_id=" + google_form_encode(Config.get_gdrive_client_id()) +
           "&client_secret=" + google_form_encode(Config.get_gdrive_client_secret());
}

#endif /* GOOGLE_OAUTH_H */
