/*
 // Copyright (c) 2021-2022 Timothy Schoen.
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
*/

#include "Statusbar.h"
#include "LookAndFeel.h"

#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "Canvas.h"
#include "Connection.h"

class LevelMeter : public Component
    , public Timer {
    int numChannels = 2;
    StatusbarSource& source;

public:
    explicit LevelMeter(StatusbarSource& statusbarSource)
        : source(statusbarSource)
    {
        startTimerHz(20);
    }

    void timerCallback() override
    {
        if (isShowing()) {
            bool needsRepaint = false;
            for (int ch = 0; ch < numChannels; ch++) {
                auto newLevel = source.level[ch].load();

                if (!std::isfinite(newLevel)) {
                    source.level[ch] = 0;
                    blocks[ch] = 0;
                    return;
                }

                float lvl = (float)std::exp(std::log(newLevel) / 3.0) * (newLevel > 0.002);
                auto numBlocks = roundToInt(totalBlocks * lvl);

                if (blocks[ch] != numBlocks) {
                    blocks[ch] = numBlocks;
                    needsRepaint = true;
                }
            }

            if (needsRepaint)
                repaint();
        }
    }

    void paint(Graphics& g) override
    {
        auto height = getHeight() / 2.0f;
        auto width = getWidth() - 8.0f;
        auto x = 4.0f;

        auto outerBorderWidth = 2.0f;
        auto spacingFraction = 0.08f;
        auto doubleOuterBorderWidth = 2.0f * outerBorderWidth;

        auto blockWidth = (width - doubleOuterBorderWidth) / static_cast<float>(totalBlocks);
        auto blockHeight = height - doubleOuterBorderWidth;
        auto blockRectWidth = (1.0f - 2.0f * spacingFraction) * blockWidth;
        auto blockRectSpacing = spacingFraction * blockWidth;
        auto c = findColour(PlugDataColour::levelMeterActiveColourId);

        for (int ch = 0; ch < numChannels; ch++) {
            auto y = ch * height;

            for (auto i = 0; i < totalBlocks; ++i) {
                if (i >= blocks[ch])
                    g.setColour(findColour(PlugDataColour::levelMeterInactiveColourId));
                else
                    g.setColour(i < totalBlocks - 1 ? c : Colours::red);

                if (i == 0 || i == totalBlocks - 1) {
                    bool curveTop = ch == 0;
                    bool curveLeft = i == 0;

                    auto roundedBlockPath = Path();
                    roundedBlockPath.addRoundedRectangle(x + outerBorderWidth + (i * blockWidth) + blockRectSpacing, y + outerBorderWidth, blockRectWidth, blockHeight, 4.0f, 4.0f, curveTop && curveLeft, curveTop && !curveLeft, !curveTop && curveLeft, !curveTop && !curveLeft);
                    g.fillPath(roundedBlockPath);
                } else {
                    g.fillRect(x + outerBorderWidth + (i * blockWidth) + blockRectSpacing, y + outerBorderWidth, blockRectWidth, blockHeight);
                }
            }
        }

        g.setColour(findColour(PlugDataColour::outlineColourId));
        g.drawRoundedRectangle(x + outerBorderWidth, outerBorderWidth, width - doubleOuterBorderWidth, getHeight() - doubleOuterBorderWidth, 4.0f, 1.0f);
    }

    int totalBlocks = 15;
    int blocks[2] = { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LevelMeter)
};

class MidiBlinker : public Component
    , public Timer {
    StatusbarSource& source;

public:
    explicit MidiBlinker(StatusbarSource& statusbarSource)
        : source(statusbarSource)
    {
        startTimer(200);
    }

    void paint(Graphics& g) override
    {
        g.setFont(Font(11));
        PlugDataLook::drawText(g, "MIDI", getLocalBounds().removeFromLeft(28), Justification::centredRight, findColour(ComboBox::textColourId));

        auto midiInRect = Rectangle<float>(38.0f, 8.0f, 15.0f, 3.0f);
        auto midiOutRect = Rectangle<float>(38.0f, 17.0f, 15.0f, 3.0f);

        g.setColour(blinkMidiIn ? findColour(PlugDataColour::levelMeterActiveColourId) : findColour(PlugDataColour::levelMeterInactiveColourId));
        g.fillRoundedRectangle(midiInRect, 1.0f);

        g.setColour(blinkMidiOut ? findColour(PlugDataColour::levelMeterActiveColourId) : findColour(PlugDataColour::levelMeterInactiveColourId));
        g.fillRoundedRectangle(midiOutRect, 1.0f);
    }

    void timerCallback() override
    {
        if (source.midiReceived != blinkMidiIn) {
            blinkMidiIn = source.midiReceived;
            repaint();
        }
        if (source.midiSent != blinkMidiOut) {
            blinkMidiOut = source.midiSent;
            repaint();
        }
    }

    bool blinkMidiIn = false;
    bool blinkMidiOut = false;
};

Statusbar::Statusbar(PluginProcessor* processor)
    : pd(processor)
{
    levelMeter = new LevelMeter(pd->statusbarSource);
    midiBlinker = new MidiBlinker(pd->statusbarSource);

    setWantsKeyboardFocus(true);

    commandLocked.referTo(pd->commandLocked);

    locked.addListener(this);
    commandLocked.addListener(this);

    oversampleSelector.setTooltip("Set oversampling");
    oversampleSelector.setName("statusbar:oversample");
    oversampleSelector.setColour(ComboBox::outlineColourId, Colours::transparentBlack);

    oversampleSelector.setButtonText(String(1 << pd->oversampling) + "x");

    oversampleSelector.onClick = [this]() {
        PopupMenu menu;
        menu.addItem(1, "1x");
        menu.addItem(2, "2x");
        menu.addItem(3, "4x");
        menu.addItem(4, "8x");

        auto* editor = pd->getActiveEditor();
        menu.showMenuAsync(PopupMenu::Options().withMinimumWidth(100).withMaximumNumColumns(1).withTargetComponent(&oversampleSelector).withParentComponent(editor),
            [this](int result) {
                if (result != 0) {
                    oversampleSelector.setButtonText(String(1 << (result - 1)) + "x");
                    pd->setOversampling(result - 1);
                }
            });
    };
    addAndMakeVisible(oversampleSelector);

    powerButton = std::make_unique<TextButton>(Icons::Power);
    lockButton = std::make_unique<TextButton>(Icons::Lock);
    connectionStyleButton = std::make_unique<TextButton>(Icons::ConnectionStyle);
    connectionPathfind = std::make_unique<TextButton>(Icons::Wand);
    presentationButton = std::make_unique<TextButton>(Icons::Presentation);
    gridButton = std::make_unique<TextButton>(Icons::Grid);

    presentationButton->setTooltip("Presentation Mode");
    presentationButton->setClickingTogglesState(true);
    presentationButton->setConnectedEdges(12);
    presentationButton->setName("statusbar:presentation");
    presentationButton->getToggleStateValue().referTo(presentationMode);

    presentationButton->onClick = [this]() {
        // When presenting we are always locked
        // A bit different from Max's presentation mode
        if (presentationButton->getToggleState()) {
            locked = var(true);
        }
    };

    addAndMakeVisible(presentationButton.get());

    powerButton->setTooltip("Mute");
    powerButton->setClickingTogglesState(true);
    powerButton->setConnectedEdges(12);
    powerButton->setName("statusbar:mute");
    addAndMakeVisible(powerButton.get());

    gridButton->setTooltip("Enable grid");
    gridButton->setClickingTogglesState(true);
    gridButton->setConnectedEdges(12);
    gridButton->setName("statusbar:grid");

    gridButton->getToggleStateValue().referTo(SettingsFile::getInstance()->getPropertyAsValue("GridEnabled"));
    addAndMakeVisible(gridButton.get());

    powerButton->onClick = [this]() { powerButton->getToggleState() ? pd->startDSP() : pd->releaseDSP(); };

    powerButton->setToggleState(pd_getdspstate(), dontSendNotification);

    lockButton->setTooltip("Edit Mode");
    lockButton->setClickingTogglesState(true);
    lockButton->setConnectedEdges(12);
    lockButton->setName("statusbar:lock");
    lockButton->getToggleStateValue().referTo(locked);
    addAndMakeVisible(lockButton.get());
    lockButton->setButtonText(locked == var(true) ? Icons::Lock : Icons::Unlock);
    lockButton->onClick = [this]() {
        if (static_cast<bool>(presentationMode.getValue())) {
            presentationMode = false;
        }
    };

    connectionStyleButton->setTooltip("Enable segmented connections");
    connectionStyleButton->setClickingTogglesState(true);
    connectionStyleButton->setConnectedEdges(12);
    connectionStyleButton->setName("statusbar:connectionstyle");
    connectionStyleButton->onClick = [this]() {
        bool segmented = connectionStyleButton->getToggleState();
        auto* editor = dynamic_cast<PluginEditor*>(pd->getActiveEditor());
        for (auto& connection : editor->getCurrentCanvas()->getSelectionOfType<Connection>()) {
            connection->setSegmented(segmented);
        }
    };

    addAndMakeVisible(connectionStyleButton.get());

    connectionPathfind->setTooltip("Find best connection path");
    connectionPathfind->setConnectedEdges(12);
    connectionPathfind->setName("statusbar:findpath");
    connectionPathfind->onClick = [this]() { dynamic_cast<ApplicationCommandManager*>(pd->getActiveEditor())->invokeDirectly(CommandIDs::ConnectionPathfind, true); };
    addAndMakeVisible(connectionPathfind.get());

    addAndMakeVisible(volumeSlider);
    volumeSlider.setTextBoxStyle(Slider::NoTextBox, false, 0, 0);

    volumeSlider.setValue(0.75);
    volumeSlider.setRange(0.0f, 1.0f);
    volumeSlider.setName("statusbar:meter");

    volumeAttachment = std::make_unique<SliderParameterAttachment>(*dynamic_cast<RangedAudioParameter*>(pd->getParameters()[0]), volumeSlider, nullptr);

    addAndMakeVisible(levelMeter);
    addAndMakeVisible(midiBlinker);

    levelMeter->toBehind(&volumeSlider);

    setSize(getWidth(), statusbarHeight);

    // Timer to make sure modifier keys are up-to-date...
    // Hoping to find a better solution for this
    startTimer(150);
}

Statusbar::~Statusbar()
{
    delete midiBlinker;
    delete levelMeter;
}

void Statusbar::attachToCanvas(Canvas* cnv)
{
    locked.referTo(cnv->locked);
    lockButton->getToggleStateValue().referTo(cnv->locked);
}

void Statusbar::valueChanged(Value& v)
{
    bool lockIcon = locked == var(true) || commandLocked == var(true);
    lockButton->setButtonText(lockIcon ? Icons::Lock : Icons::Unlock);

    if (v.refersToSameSourceAs(commandLocked)) {
        auto c = static_cast<bool>(commandLocked.getValue()) ? findColour(PlugDataColour::toolbarActiveColourId) : findColour(PlugDataColour::toolbarTextColourId);
        lockButton->setColour(PlugDataColour::toolbarTextColourId, c);
    }
}

void Statusbar::paint(Graphics& g)
{
    g.setColour(findColour(PlugDataColour::outlineColourId));
    g.drawLine(0.0f, 0.5f, static_cast<float>(getWidth()), 0.5f);

    // Makes sure it gets updated on theme change
    auto c = static_cast<bool>(commandLocked.getValue()) ? findColour(PlugDataColour::toolbarActiveColourId) : findColour(PlugDataColour::toolbarTextColourId);
    lockButton->setColour(PlugDataColour::toolbarTextColourId, c);
}

void Statusbar::resized()
{
    int pos = 0;
    auto position = [this, &pos](int width, bool inverse = false) -> int {
        int result = 8 + pos;
        pos += width + 3;
        return inverse ? getWidth() - pos : result;
    };

    lockButton->setBounds(position(getHeight()), 0, getHeight(), getHeight());
    presentationButton->setBounds(position(getHeight()), 0, getHeight(), getHeight());

    position(3); // Seperator

    connectionStyleButton->setBounds(position(getHeight()), 0, getHeight(), getHeight());
    connectionPathfind->setBounds(position(getHeight()), 0, getHeight(), getHeight());

    position(3); // Seperator

    gridButton->setBounds(position(getHeight()), 0, getHeight(), getHeight());

    pos = 0; // reset position for elements on the left

    powerButton->setBounds(position(getHeight(), true), 0, getHeight(), getHeight());

    int levelMeterPosition = position(100, true);
    levelMeter->setBounds(levelMeterPosition, 2, 100, getHeight() - 4);
    volumeSlider.setBounds(levelMeterPosition, 2, 100, getHeight() - 4);

    // Offset to make text look centred
    oversampleSelector.setBounds(position(getHeight(), true) + 3, 0, getHeight(), getHeight());

    midiBlinker->setBounds(position(55, true), 0, 55, getHeight());
}

void Statusbar::modifierKeysChanged(ModifierKeys const& modifiers)
{
    auto* editor = dynamic_cast<PluginEditor*>(pd->getActiveEditor());

    commandLocked = modifiers.isCommandDown() && locked.getValue() == var(false);

    if (auto* cnv = editor->getCurrentCanvas()) {
        if (cnv->didStartDragging || cnv->isDraggingLasso || static_cast<bool>(cnv->presentationMode.getValue())) {
            return;
        }

        for (auto* object : cnv->objects) {
            object->showIndex(modifiers.isAltDown());
        }
    }
}

void Statusbar::timerCallback()
{
    modifierKeysChanged(ModifierKeys::getCurrentModifiersRealtime());
}

StatusbarSource::StatusbarSource()
{
    level[0] = 0.0f;
    level[1] = 0.0f;
}

static bool hasRealEvents(MidiBuffer& buffer)
{
    return std::any_of(buffer.begin(), buffer.end(),
        [](auto const& event) {
            return !event.getMessage().isSysEx();
        });
}

void StatusbarSource::processBlock(AudioBuffer<float> const& buffer, MidiBuffer& midiIn, MidiBuffer& midiOut, int channels)
{
    auto const* const* channelData = buffer.getArrayOfReadPointers();

    if (channels == 1) {
        level[1] = 0;
    } else if (channels == 0) {
        level[0] = 0;
        level[1] = 0;
    }

    for (int ch = 0; ch < channels; ch++) {
        // TODO: this logic for > 2 channels makes no sense!!
        auto localLevel = level[ch & 1].load();

        for (int n = 0; n < buffer.getNumSamples(); n++) {
            float s = std::abs(channelData[ch][n]);

            float const decayFactor = 0.99992f;

            if (s > localLevel)
                localLevel = s;
            else if (localLevel > 0.001f)
                localLevel *= decayFactor;
            else
                localLevel = 0;
        }

        level[ch & 1] = localLevel;
    }

    auto now = Time::getCurrentTime();

    auto hasInEvents = hasRealEvents(midiIn);
    auto hasOutEvents = hasRealEvents(midiOut);

    if (!hasInEvents && (now - lastMidiIn).inMilliseconds() > 700) {
        midiReceived = false;
    } else if (hasInEvents) {
        midiReceived = true;
        lastMidiIn = now;
    }

    if (!hasOutEvents && (now - lastMidiOut).inMilliseconds() > 700) {
        midiSent = false;
    } else if (hasOutEvents) {
        midiSent = true;
        lastMidiOut = now;
    }
}

void StatusbarSource::prepareToPlay(int nChannels)
{
    numChannels = nChannels;
}
