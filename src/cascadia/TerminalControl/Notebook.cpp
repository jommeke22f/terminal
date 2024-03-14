// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "Notebook.h"
#include "TermControl.h"
#include "Notebook.g.cpp"
using namespace ::Microsoft::Console::Types;
using namespace ::Microsoft::Console::VirtualTerminal;
using namespace ::Microsoft::Terminal::Core;
using namespace winrt::Windows::Graphics::Display;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Input;
using namespace winrt::Windows::UI::Xaml::Automation::Peers;
using namespace winrt::Windows::UI::Core;
using namespace winrt::Windows::UI::ViewManagement;
using namespace winrt::Windows::UI::Input;
using namespace winrt::Windows::System;
using namespace winrt::Windows::ApplicationModel::DataTransfer;
using namespace winrt::Windows::Storage::Streams;

namespace winrt
{
    namespace MUX = Microsoft::UI::Xaml;
    namespace WUX = Windows::UI::Xaml;
    using IInspectable = Windows::Foundation::IInspectable;
}

namespace winrt::Microsoft::Terminal::Control::implementation
{
    Notebook::Notebook(Control::IControlSettings settings,
                       Control::IControlAppearance unfocusedAppearance,
                       TerminalConnection::ITerminalConnection connection) :
        _settings{ settings },
        _unfocusedAppearance{ unfocusedAppearance },
        _connection{ connection }
    {
        _terminal = std::make_shared<::Microsoft::Terminal::Core::Terminal>();

        _terminal->NewPrompt({ get_weak(), &Notebook::_handleNewPrompt });

        _fork(0);
    }

    void Notebook::_handleNewPrompt(const ::ScrollMark& mark)
    {
        _expectedPrompts--;
        if (_expectedPrompts > 0)
            return;
        if (_gotFirstMark)
        {
            _fork(mark.start.y);
        }
        else
        {
            _gotFirstMark = true;
        }
    }

    Windows::Foundation::Collections::IVector<Microsoft::Terminal::Control::NotebookBlock> Notebook::Blocks() const
    {
        return nullptr;
    }

    Control::NotebookBlock Notebook::ActiveBlock() const
    {
        const auto active{ _activeBlock() };
        return active ? *active : Control::NotebookBlock{ nullptr };
    }

    winrt::com_ptr<implementation::NotebookBlock> Notebook::_activeBlock() const
    {
        if (_blocks.empty())
        {
            return nullptr;
        }
        return *_blocks.rbegin();
    }

    void Notebook::SendCommands(const winrt::hstring& commands)
    {
        auto numCarriageReturns = std::count(commands.begin(), commands.end(), L'\r');

        auto active = _activeBlock();
        if (numCarriageReturns > 0)
        {
            _expectedPrompts = numCarriageReturns;

            active->State(BlockState::Running);
            // BAD just make it not a winrt property you donkey
            active->StateChanged.raise(*active, nullptr);
        }

        active->Control().SendInput(commands);
    }

    winrt::fire_and_forget Notebook::_fork(const til::CoordType start)
    {
        if (_currentlyCreating.exchange(true))
        {
            co_return;
        }
        auto active = _activeBlock();

        if (active && !active->Control().Dispatcher().HasThreadAccess())
        {
            co_await wil::resume_foreground(active->Control().Dispatcher());
        }
        _forkOnUIThread(start);
    }

    void Notebook::_forkOnUIThread(const til::CoordType start)
    {
        auto exit = wil::scope_exit([&] { _currentlyCreating.exchange(false); });

        auto active = _activeBlock();
        if (active)
        {
            active->State(BlockState::Finished);
            // BAD just make it not a winrt property you donkey
            active->StateChanged.raise(*active, nullptr);

            auto core{ active->core };
            auto* renderData{ active->renderData.get() };
            auto activeControl = active->Control();

            // First, important. Set the bottom, so it thinks it has ended,
            renderData->SetBottom(start - 1);

            // Get how tall the viewport is with the new bottom, under lock.
            renderData->LockConsole();
            auto blockRenderViewport = renderData->GetViewport();
            renderData->UnlockConsole();

            const auto rows = blockRenderViewport.Height();
            const auto fakePixelHeight = 16 + 16 * rows;

            const auto pixels = core->ViewInPixels(blockRenderViewport.ToExclusive());
            const auto scaleFactor = DisplayInformation::GetForCurrentView().RawPixelsPerViewPixel();
            const auto controlHeightDips = activeControl.ActualHeight();
            const auto viewHeightDips = pixels.height() * scaleFactor;
            const auto fakeHeightDips = fakePixelHeight * scaleFactor;
            controlHeightDips;
            viewHeightDips;
            fakeHeightDips;

            auto t{ WUX::ThicknessHelper::FromLengths(0 /*r.left*/,
                                                      0 /*r.top*/,
                                                      0 /*r.right*/,
                                                      -(controlHeightDips - viewHeightDips) /*r.bottom*/) };
            activeControl.Margin(t);
            // activeControl.Height(fakeHeightDips);
            activeControl.Connection(nullptr);
        }
        _createNewBlock(start);
    }

    void Notebook::_createNewBlock(const til::CoordType start)
    {
        auto newBlock = winrt::make_self<implementation::NotebookBlock>();
        newBlock->renderData = std::make_unique<::Microsoft::Terminal::Core::BlockRenderData>(*_terminal, start);

        ControlData data{
            .terminal = _terminal,
            .renderData = newBlock->renderData.get(),
            .connection = _connection,
        };

        newBlock->core = winrt::make_self<implementation::ControlCore>(_settings, _unfocusedAppearance, data);
        auto interactivityOne = winrt::make_self<implementation::ControlInteractivity>(_settings, _unfocusedAppearance, newBlock->core);
        newBlock->Control(winrt::make<implementation::TermControl>(*interactivityOne));

        _blocks.push_back(std::move(newBlock));

        // NewBlock.raise(*this, ActiveBlock());
    }

}