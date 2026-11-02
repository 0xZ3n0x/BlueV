// SPDX-License-Identifier: GPL-3.0-or-later
// A small JSON writer: it escapes strings and places the commas and colons, so
// the caller only says what the structure is:
//
//   Json j;
//   j.beginObject().key("on").value(true).key("n").value(3).endObject();
//   j.take();      // {"on":true,"n":3}
#pragma once

#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bluev
{

class Json
{
  public:
    Json& beginObject()
    {
        return open('{');
    }
    Json& endObject()
    {
        return close('}');
    }
    Json& beginArray()
    {
        return open('[');
    }
    Json& endArray()
    {
        return close(']');
    }

    Json& key(std::string_view name)
    {
        separate();
        appendString(name);
        m_text += ':';
        m_after_key = true;
        return *this;
    }

    Json& value(std::string_view text)
    {
        separate();
        appendString(text);
        return *this;
    }
    Json& value(const char* text)
    {
        return value(std::string_view(text));
    }
    Json& value(bool flag)
    {
        separate();
        m_text += flag ? "true" : "false";
        return *this;
    }
    Json& value(long number)
    {
        separate();
        m_text += std::to_string(number);
        return *this;
    }
    Json& value(int number)
    {
        return value(static_cast<long>(number));
    }
    Json& value(unsigned number)
    {
        return value(static_cast<long>(number));
    }

    std::string take()
    {
        return std::move(m_text);
    }

  private:
    Json& open(char bracket)
    {
        separate();
        m_text += bracket;
        m_first.push_back(true);
        return *this;
    }

    Json& close(char bracket)
    {
        m_text += bracket;
        if (!m_first.empty())
        {
            m_first.pop_back();
        }
        return *this;
    }

    // a comma before every element but the first (a value after a key
    // needs none)
    void separate()
    {
        if (m_after_key)
        {
            m_after_key = false;
            return;
        }
        if (m_first.empty())
        {
            return;
        }
        if (m_first.back())
        {
            m_first.back() = false;
        }
        else
        {
            m_text += ',';
        }
    }

    void appendString(std::string_view text)
    {
        m_text += '"';
        for (unsigned char c : text)
        {
            switch (c)
            {
            case '"':
                m_text += "\\\"";
                break;
            case '\\':
                m_text += "\\\\";
                break;
            case '\n':
                m_text += "\\n";
                break;
            case '\r':
                m_text += "\\r";
                break;
            case '\t':
                m_text += "\\t";
                break;
            default:
                if (c < 0x20)
                {
                    char escape[8];

                    std::snprintf(escape, sizeof escape, "\\u%04x", c);
                    m_text += escape;
                }
                else
                {
                    m_text += static_cast<char>(c);
                }
            }
        }
        m_text += '"';
    }

    std::string m_text;
    std::vector<bool> m_first; // one entry per open object/array
    bool m_after_key = false;
};

} // namespace bluev
