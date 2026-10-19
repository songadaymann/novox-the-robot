// Standalone lifecycle adapted from JUCE 8.0.15 StandaloneFilterApp.
// Copyright Raw Material Software Limited. AGPLv3, same as the plugin.
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
using namespace juce;

class NovoxStandaloneWindow : public StandaloneFilterWindow {
public:
    NovoxStandaloneWindow(const String& title, Colour colour, std::unique_ptr<StandalonePluginHolder> holder)
        : StandaloneFilterWindow(title,colour,std::move(holder)) {
#if JUCE_MAC
        setUsingNativeTitleBar(true);
        hideOptionsButton();
#endif
    }
    void resized() override {
        StandaloneFilterWindow::resized();
#if JUCE_MAC
        hideOptionsButton();
#endif
    }
private:
    void hideOptionsButton() {
        for (auto* child : getChildren())
            if (auto* button = dynamic_cast<TextButton*>(child))
                if (button->getButtonText()=="Options") button->setVisible(false);
    }
};

class NovoxStandaloneApp final : public JUCEApplication, private MenuBarModel
{
public:
    NovoxStandaloneApp()
    {
        PropertiesFile::Options options;

        options.applicationName     = CharPointer_UTF8 (JucePlugin_Name);
        options.filenameSuffix      = ".settings";
        options.osxLibrarySubFolder = "Application Support";
       #if JUCE_LINUX || JUCE_BSD
        options.folderName          = "~/.config";
       #else
        options.folderName          = "";
       #endif

        appProperties.setStorageParameters (options);
    }

    const String getApplicationName() override              { return CharPointer_UTF8 (JucePlugin_Name); }
    const String getApplicationVersion() override           { return JucePlugin_VersionString; }
    bool moreThanOneInstanceAllowed() override              { return true; }
    void anotherInstanceStarted (const String&) override    {}

    StandaloneFilterWindow* createWindow()
    {
        if (Desktop::getInstance().getDisplays().displays.isEmpty())
        {
            // No displays are available, so no window will be created!
            jassertfalse;
            return nullptr;
        }

        return new NovoxStandaloneWindow (getApplicationName(),
                                           LookAndFeel::getDefaultLookAndFeel().findColour (ResizableWindow::backgroundColourId),
                                           createPluginHolder());
    }

    std::unique_ptr<StandalonePluginHolder> createPluginHolder()
    {
        constexpr auto autoOpenMidiDevices =
       #if (JUCE_ANDROID || JUCE_IOS) && ! JUCE_DONT_AUTO_OPEN_MIDI_DEVICES_ON_MOBILE
                true;
       #else
                false;
       #endif


       #ifdef JucePlugin_PreferredChannelConfigurations
        constexpr StandalonePluginHolder::PluginInOuts channels[] { JucePlugin_PreferredChannelConfigurations };
        const Array<StandalonePluginHolder::PluginInOuts> channelConfig (channels, juce::numElementsInArray (channels));
       #else
        const Array<StandalonePluginHolder::PluginInOuts> channelConfig;
       #endif

        return std::make_unique<StandalonePluginHolder> (appProperties.getUserSettings(),
                                                         false,
                                                         String{},
                                                         nullptr,
                                                         channelConfig,
                                                         autoOpenMidiDevices);
    }

    //==============================================================================
    void initialise (const String&) override
    {
        mainWindow = rawToUniquePtr (createWindow());

        if (mainWindow != nullptr)
        {
           #if JUCE_STANDALONE_FILTER_WINDOW_USE_KIOSK_MODE
            Desktop::getInstance().setKioskModeComponent (mainWindow.get(), false);
           #endif

            mainWindow->setVisible (true);
#if JUCE_MAC
            MenuBarModel::setMacMainMenu(this);
#endif
        }
        else
        {
            pluginHolder = createPluginHolder();
        }
    }

    void shutdown() override
    {
#if JUCE_MAC
        MenuBarModel::setMacMainMenu(nullptr);
#endif
        pluginHolder = nullptr;
        mainWindow = nullptr;
        appProperties.saveIfNeeded();
    }

    //==============================================================================
    void systemRequestedQuit() override
    {
        if (pluginHolder != nullptr)
            pluginHolder->savePluginState();

        if (mainWindow != nullptr)
            mainWindow->pluginHolder->savePluginState();

        if (ModalComponentManager::getInstance()->cancelAllModalComponents())
        {
            Timer::callAfterDelay (100, []()
            {
                if (auto app = JUCEApplicationBase::getInstance())
                    app->systemRequestedQuit();
            });
        }
        else
        {
            quit();
        }
    }

protected:
    ApplicationProperties appProperties;
    std::unique_ptr<StandaloneFilterWindow> mainWindow;

private:
    StringArray getMenuBarNames() override { return {"Options"}; }
    PopupMenu getMenuForIndex(int, const String&) override {
        PopupMenu menu;
        menu.addItem(1,"Audio/MIDI Settings...");
        menu.addSeparator();
        menu.addItem(2,"Save current state...");
        menu.addItem(3,"Load a saved state...");
        menu.addSeparator();
        menu.addItem(4,"Reset to default state");
        return menu;
    }
    void menuItemSelected(int item, int) override {
        if (mainWindow) mainWindow->handleMenuResult(item);
    }
    std::unique_ptr<StandalonePluginHolder> pluginHolder;
};

JUCE_CREATE_APPLICATION_DEFINE(NovoxStandaloneApp)
