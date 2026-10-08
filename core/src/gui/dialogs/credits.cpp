#include <gui/dialogs/credits.h>
#include <imgui.h>
#include <gui/icons.h>
#include <gui/style.h>
#include <config.h>
#include <credits.h>
#include <version.h>

namespace credits {
    ImFont* bigFont;
    ImVec2 imageSize(128.0f, 128.0f);

    void init() {
        imageSize = ImVec2(128.0f * style::uiScale, 128.0f * style::uiScale);
    }

    // GPL-3.0: where the corresponding source of this build lives, and the licenses of what it
    // bundles. Shown in the credits popup and in the menu (About), which touch mode uses.
    void drawLicenseText() {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(APP_NAME " " APP_VERSION " by Zoltan Doczi (z2labs.io) is an unofficial fork of SDR++ " VERSION_STR
                               " by Alexandre Rouma (ON5RYZ) and contributors. It is not an official SDR++ release.");
        ImGui::TextUnformatted("License: GNU General Public License v3.0 (GPL-3.0-or-later). No warranty.");
        ImGui::TextUnformatted("Source code of this build:");
        ImGui::BulletText("App: github.com/z2labs/SDRPlusPlus (branch esp-sdr), based on github.com/AlexandreRouma/SDRPlusPlus");
        ImGui::BulletText("ESP32-S3 source module: github.com/z2labs/sdrpp-esp-sdr-source");
        ImGui::BulletText("Bundled ESP32-S3 firmware: github.com/zodoczi/esp-sdr (branch iq-smooth), based on ESP-SDR by "
                          "Florian Euchner / ESPARGOS (GPL-3.0-or-later), with ESP-IDF and ESP-DSP (Apache-2.0)");
        ImGui::BulletText("Libraries: see the SDR++ credits (volk, FFTW, Dear ImGui, json, librtlsdr, libusb and others)");
        ImGui::PopTextWrapPos();
    }

    void show() {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0f, 20.0f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
        ImVec2 dispSize = ImGui::GetIO().DisplaySize;
        ImVec2 center = ImVec2(dispSize.x / 2.0f, dispSize.y / 2.0f);
        ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::OpenPopup("Credits");
        ImGui::BeginPopupModal("Credits", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove);

        ImGui::PushFont(style::hugeFont);
        ImGui::TextUnformatted(APP_NAME "          ");
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::Image(icons::LOGO, imageSize);
        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::Spacing();

        ImGui::TextUnformatted("SDR++, the base of " APP_NAME ", is brought to you by Alexandre Rouma (ON5RYZ) with the help of\n\n");

        ImGui::Columns(4, "CreditColumns", true);

        ImGui::TextUnformatted("Contributors");
        for (int i = 0; i < sdrpp_credits::contributorCount; i++) {
            ImGui::BulletText("%s", sdrpp_credits::contributors[i]);
        }

        ImGui::NextColumn();
        ImGui::TextUnformatted("Libraries");
        for (int i = 0; i < sdrpp_credits::libraryCount; i++) {
            ImGui::BulletText("%s", sdrpp_credits::libraries[i]);
        }

        ImGui::NextColumn();
        ImGui::TextUnformatted("Hardware Donators");
        for (int i = 0; i < sdrpp_credits::hardwareDonatorCount; i++) {
            ImGui::BulletText("%s", sdrpp_credits::hardwareDonators[i]);
        }

        ImGui::NextColumn();
        ImGui::TextUnformatted("Patrons");
        for (int i = 0; i < sdrpp_credits::patronCount; i++) {
            ImGui::BulletText("%s", sdrpp_credits::patrons[i]);
        }

        ImGui::Columns(1, "CreditColumnsEnd", true);

        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::Spacing();
        drawLicenseText();
        ImGui::Spacing();
        ImGui::TextUnformatted(APP_NAME " " APP_VERSION ", based on SDR++ v" VERSION_STR " (Built at " __TIME__ ", " __DATE__ ")");

        ImGui::EndPopup();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
    }
}