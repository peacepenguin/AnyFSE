// MIT License
//
// Copyright (c) 2025 Artem Shpynov
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//

#pragma once

#include <windows.h>
#include <cmath>

namespace FluentDesign
{
    // Eases a scroll position toward a target. Drive it from a WM_TIMER at SmoothScroller::TimerIntervalMs.
    class SmoothScroller
    {
        static constexpr double TauMs = 70.0;
        static constexpr ULONGLONG MaxStepMs = 50;

        double m_pos = 0;
        int m_target = 0;
        ULONGLONG m_lastTick = 0;

    public:
        static constexpr UINT TimerIntervalMs = 16;

        void Reset(int pos)
        {
            m_pos = pos;
            m_target = pos;
            m_lastTick = 0;
        }

        void SetTarget(int target)
        {
            m_target = target;
            if (!m_lastTick)
            {
                m_lastTick = GetTickCount64();
            }
        }

        int Target() const { return m_target; }

        bool Active() const { return m_lastTick != 0; }

        // Advances toward the target and returns the new integer position.
        int Step()
        {
            const ULONGLONG now = GetTickCount64();
            ULONGLONG dt = now - m_lastTick;
            m_lastTick = now;
            if (dt < 1) dt = 1;
            if (dt > MaxStepMs) dt = MaxStepMs;

            m_pos += (m_target - m_pos) * (1.0 - std::exp(-(double)dt / TauMs));
            if (std::fabs(m_target - m_pos) < 0.5)
            {
                m_pos = m_target;
                m_lastTick = 0;
            }
            return (int)std::lround(m_pos);
        }
    };
}
