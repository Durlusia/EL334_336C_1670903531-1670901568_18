#include "MK64F12.h"
#include <stdint.h>

// ============================================================================
// 1. Music Note Frequencies (Hz) and Melody Structure
// ============================================================================
#define NOTE_REST     0     // Silence
#define NOTE_C4       262
#define NOTE_CS4      277
#define NOTE_D4       294
#define NOTE_DS4      311
#define NOTE_E4       330
#define NOTE_F4       349
#define NOTE_FS4      370
#define NOTE_G4       392
#define NOTE_GS4      415
#define NOTE_A4       440
#define NOTE_AS4      466
#define NOTE_B4       494
#define NOTE_C5       523
#define NOTE_CS5      554
#define NOTE_D5       587
#define NOTE_DS5      622
#define NOTE_E5       659
#define NOTE_F5       698
#define NOTE_FS5      740
#define NOTE_G5       784
#define NOTE_GS5      831
#define NOTE_A5       880
#define NOTE_AS5      932
#define NOTE_B5       988
#define NOTE_C6       1047

typedef struct {
    uint16_t freq_hz;       // Sound frequency (Hz)
    uint16_t duration_ms;   // Note duration (ms)
} Note_t;

// Helper macro to compute melody note count automatically
#define MELODY_LEN(m) ((uint8_t)(sizeof(m) / sizeof(Note_t)))

// Arcade Sound Effects & Melodies
const Note_t MELODY_START[] = {
    {NOTE_E5, 110}, {NOTE_E5, 110}, {NOTE_REST, 60}, {NOTE_E5, 110},
    {NOTE_REST, 60}, {NOTE_C5, 110}, {NOTE_E5, 180}, {NOTE_G5, 300},
    {NOTE_G4, 300}
};

const Note_t MELODY_HIT[] = {
    {NOTE_B5, 65}, {NOTE_C6, 130}
};

const Note_t MELODY_MISS[] = {
    {NOTE_A4, 80}, {NOTE_E4, 150}
};

const Note_t MELODY_BOMB[] = {
    {NOTE_G4, 90}, {NOTE_FS4, 90}, {NOTE_F4, 90}, {NOTE_E4, 240}
};

const Note_t MELODY_GAMEOVER[] = {
    {NOTE_C5, 160}, {NOTE_REST, 40}, {NOTE_G4, 160}, {NOTE_REST, 40},
    {NOTE_E4, 220}, {NOTE_A4, 150},  {NOTE_B4, 150}, {NOTE_A4, 150},
    {NOTE_GS4, 180},{NOTE_AS4, 180}, {NOTE_GS4, 180},
    {NOTE_G4, 140}, {NOTE_F4, 140},  {NOTE_G4, 480}
};

// ============================================================================
// 2. Game States and Global Variables
// ============================================================================
#define STATE_IDLE      0   // Waiting for S1 to start
#define STATE_PLAYING   1   // Game in progress
#define STATE_GAMEOVER  2   // Time up, showing final score

volatile uint8_t  game_state = STATE_IDLE;
volatile uint8_t  time_left_sec = 30;       // 30-second countdown
volatile uint8_t  score = 0;                // Player score (0 - 99)
volatile uint8_t  current_target = 0;       // 0=Empty, 1=D1, 2=D2, 3=D3, 4=D4(Bomb)

volatile uint16_t sec_tick_ms = 0;          // 1-second counter for countdown
volatile uint16_t target_timer_ms = 0;      // Duration timer for mole/bomb
volatile uint16_t gameover_lock_ms = 0;     // Prevent accidental restart on game over
volatile uint32_t rand_seed = 2026;         // Seed for random target generator

volatile uint8_t  display_digits[4] = {3, 0, 0, 0}; // 7-Segment digits
volatile uint8_t  current_digit_idx = 0;            // Active digit index (0-3)

volatile uint16_t debounce_timer_ms = 0;    // Button debounce counter
volatile uint16_t blink_timer_ms = 0;       // LED blink counter
volatile uint8_t  blink_state = 0;          // LED blink state (0 or 1)

// Variables for melody playback
volatile const Note_t *active_melody = 0;
volatile uint8_t  melody_len = 0;
volatile uint8_t  melody_idx = 0;
volatile uint16_t note_timer_ms = 0;

// 7-Segment lookup table for Common Anode (Active Low: 0 = ON, 1 = OFF)
const uint8_t SEGMENT_MAP[10] = {
    0xC0, 0xF9, 0xA4, 0xB0, 0x99, 0x92, 0x82, 0xF8, 0x80, 0x90
};

// Digit select table for 7-Segment (Left to Right)
const uint8_t DIGIT_SELECT[4] = {0xF1, 0xF2, 0xF4, 0xF8};

// ============================================================================
// 3. Hardware Initialization Sub-functions
// ============================================================================

// Calculate current Bus Clock frequency for FlexTimer (FTM0)
uint32_t Get_BusClock_Hz(void) {
    uint32_t outdiv1 = ((SIM->CLKDIV1 & SIM_CLKDIV1_OUTDIV1_MASK) >> SIM_CLKDIV1_OUTDIV1_SHIFT) + 1;
    uint32_t outdiv2 = ((SIM->CLKDIV1 & SIM_CLKDIV1_OUTDIV2_MASK) >> SIM_CLKDIV1_OUTDIV2_SHIFT) + 1;
    return (SystemCoreClock * outdiv1) / outdiv2;
}

void Init_GPIO_Hardware(void) {
    // Enable clock for Port A, B, C, D
    SIM->SCGC5 |= SIM_SCGC5_PORTA_MASK | SIM_SCGC5_PORTB_MASK |
                  SIM_SCGC5_PORTC_MASK | SIM_SCGC5_PORTD_MASK;

    // Setup Buzzer (PTA1 = D3) as High-Impedance (Hi-Z) to block 5V leakage
    PORTA->PCR[1] = PORT_PCR_MUX(1);
    PTA->PCOR = (1U << 1);   // Prepare logic 0
    PTA->PDDR &= ~(1U << 1); // Set as Input (Hi-Z) to keep buzzer OFF at start

    // Setup LEDs D1-D4 on Shield (PTD1, PTD3, PTD2, PTD0)
    PORTD->PCR[0] = PORT_PCR_MUX(1);
    PORTD->PCR[1] = PORT_PCR_MUX(1);
    PORTD->PCR[2] = PORT_PCR_MUX(1);
    PORTD->PCR[3] = PORT_PCR_MUX(1);
    PTD->PDDR |= (1U << 0) | (1U << 1) | (1U << 2) | (1U << 3);
    PTD->PSOR = (1U << 0) | (1U << 1) | (1U << 2) | (1U << 3); // Turn OFF all LEDs

    // Setup 7-Segment Shift Register 74HC595 pins
    PORTB->PCR[23] = PORT_PCR_MUX(1); // LATCH (D4)
    PORTC->PCR[3]  = PORT_PCR_MUX(1); // CLK   (D7)
    PORTC->PCR[12] = PORT_PCR_MUX(1); // DATA  (D8)
    PTB->PDDR |= (1U << 23);
    PTC->PDDR |= (1U << 3) | (1U << 12);
}

void Init_Switch_Interrupt(void) {
    // S1 = PTB3 (A1), S2 = PTB10 (A2), S3 = PTB11 (A3) with Pull-up and Falling Edge Interrupt
    uint32_t pcr_config = PORT_PCR_MUX(1) | PORT_PCR_PE_MASK |
                          PORT_PCR_PS_MASK | PORT_PCR_IRQC(0xA);

    PORTB->PCR[3]  = pcr_config;
    PORTB->PCR[10] = pcr_config;
    PORTB->PCR[11] = pcr_config;
    PTB->PDDR &= ~((1U << 3) | (1U << 10) | (1U << 11));

    PORTB->ISFR = (1U << 3) | (1U << 10) | (1U << 11);
    NVIC_EnableIRQ(PORTB_IRQn);
}

// Setup SysTick Timer for 1 ms interrupt (System base tick)
void Init_SysTick_Timer(void) {
    SysTick->LOAD = (SystemCoreClock / 1000UL) - 1; // Load value for 1 ms
    SysTick->VAL  = 0;                              // Reset current counter
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk |    // Use Core Clock
                    SysTick_CTRL_TICKINT_Msk   |    // Enable SysTick Interrupt
                    SysTick_CTRL_ENABLE_Msk;        // Start SysTick Timer
}

// Setup FlexTimer 0 (FTM0) for generating Buzzer Melody frequencies
void Init_FlexTimer_FTM0(void) {
    SIM->SCGC6 |= SIM_SCGC6_FTM0_MASK; // Enable clock for FTM0
    FTM0->MODE |= FTM_MODE_WPDIS_MASK; // Disable write protection
    FTM0->SC    = 0x00;                // Stop timer during setup
    FTM0->CNTIN = 0;                   // Initial counter value = 0
    FTM0->CNT   = 0;                   // Clear current counter

    NVIC_EnableIRQ(FTM0_IRQn);         // Enable FTM0 interrupt in NVIC
}

// ============================================================================
// 4. Buzzer Melody Sub-functions (using FTM0)
// ============================================================================

void Melody_Set_Frequency(uint16_t freq_hz) {
    if (freq_hz == 0) {
        FTM0->SC = 0x00;         // Stop FTM0
        PTA->PDDR &= ~(1U << 1); // Set pin as Input (Hi-Z) to mute buzzer
    } else {
        // Use Prescaler = 8 (PS = 3) so MOD fits in 16-bit counter
        uint32_t ftm_clock = Get_BusClock_Hz() / 8UL;
        uint32_t half_period_mod = (ftm_clock / (2UL * freq_hz)) - 1;

        FTM0->SC  = 0x00;
        FTM0->CNT = 0;
        FTM0->MOD = (uint16_t)half_period_mod;

        // Start FTM0 with System Clock (CLKS=1), Prescaler /8 (PS=3), and Overflow Interrupt
        FTM0->SC  = FTM_SC_CLKS(1) | FTM_SC_PS(3) | FTM_SC_TOIE_MASK;
    }
}

void Melody_Play(const Note_t *melody, uint8_t length) {
    note_timer_ms = 0;
    active_melody = melody;
    melody_len    = length;
    melody_idx    = 0;

    if (length > 0) {
        Melody_Set_Frequency(active_melody[0].freq_hz);
        note_timer_ms = active_melody[0].duration_ms;
    }
}

void Melody_Update_Tick(void) {
    if (note_timer_ms > 0) {
        note_timer_ms--;

        // Brief 15 ms silence gap before note ends so consecutive notes articulate clearly
        if (note_timer_ms == 15) {
            Melody_Set_Frequency(NOTE_REST);
        }

        if (note_timer_ms == 0) {
            melody_idx++;
            if (melody_idx < melody_len) {
                Melody_Set_Frequency(active_melody[melody_idx].freq_hz);
                note_timer_ms = active_melody[melody_idx].duration_ms;
            } else {
                Melody_Set_Frequency(NOTE_REST);
            }
        }
    }
}

// ============================================================================
// 5. Display and LED Control Sub-functions
// ============================================================================

void ShiftRegister_WriteByte(uint8_t data) {
    for (int i = 7; i >= 0; i--) {
        if (data & (1U << i)) {
            PTC->PSOR = (1U << 12);
        } else {
            PTC->PCOR = (1U << 12);
        }
        PTC->PCOR = (1U << 3);
        PTC->PSOR = (1U << 3);
    }
}

void Display_7Segment_Scan(uint8_t digit_idx) {
    uint8_t seg_code = SEGMENT_MAP[display_digits[digit_idx]];
    if (digit_idx == 1) {
        seg_code &= 0x7F; // Turn on decimal point (DP) to separate Time and Score
    }

    PTB->PCOR = (1U << 23);
    ShiftRegister_WriteByte(seg_code);
    ShiftRegister_WriteByte(DIGIT_SELECT[digit_idx]);
    PTB->PSOR = (1U << 23);
}

// Display Time (left 2 digits) and Score (right 2 digits)
void Update_Display_Values(uint8_t time_sec, uint8_t current_score) {
    display_digits[0] = (time_sec / 10) % 10;
    display_digits[1] = time_sec % 10;
    display_digits[2] = (current_score / 10) % 10;
    display_digits[3] = current_score % 10;
}

// Control Shield LEDs D1-D4 based on game state and active target
void Control_Game_LEDs(uint8_t state, uint8_t target, uint8_t blink) {
    PTD->PSOR = (1U << 0) | (1U << 1) | (1U << 2) | (1U << 3); // Turn OFF all LEDs first

    if (state == STATE_IDLE) {
        // Standby mode: blink D1 to prompt player to press S1 to start
        if (blink) {
            PTD->PCOR = (1U << 1); // D1 ON
        }
    } else if (state == STATE_PLAYING) {
        // Light up the active mole or bomb LED
        if (target == 1)      PTD->PCOR = (1U << 1); // D1 (Mole 1)
        else if (target == 2) PTD->PCOR = (1U << 3); // D2 (Mole 2)
        else if (target == 3) PTD->PCOR = (1U << 2); // D3 (Mole 3)
        else if (target == 4) PTD->PCOR = (1U << 0); // D4 (BOMB! Do not press)
    } else if (state == STATE_GAMEOVER) {
        // Game Over: blink all 4 LEDs together
        if (blink) {
            PTD->PCOR = (1U << 0) | (1U << 1) | (1U << 2) | (1U << 3);
        }
    }
}

// ============================================================================
// 6. Whack-a-Mole Game Logic Sub-functions
// ============================================================================

// Generate random target: 1-3 = Mole (82% chance), 4 = Bomb (18% chance)
uint8_t Get_Random_Target(void) {
    rand_seed = (rand_seed * 1103515245UL + 12345UL) ^ SysTick->VAL;
    uint8_t r = (rand_seed >> 16) % 11;

    if (r < 3)      return 1; // Mole 1 (D1)
    else if (r < 6) return 2; // Mole 2 (D2)
    else if (r < 9) return 3; // Mole 3 (D3)
    else            return 4; // Bomb (D4)
}

// Calculate mole stay duration based on current score (faster as score increases)
uint16_t Get_Mole_Duration_Ms(uint8_t current_score) {
    uint16_t reduction = current_score * 35;
    if (reduction >= 750) {
        return 450; // Fastest speed limit (450 ms)
    }
    return 1200 - reduction; // Starts at 1200 ms
}

// Start a new 30-second game
void Game_Start(void) {
    score = 0;
    time_left_sec = 30;
    sec_tick_ms = 0;
    current_target = 0;
    target_timer_ms = 500; // First mole appears after 500 ms
    game_state = STATE_PLAYING;
    Melody_Play(MELODY_START, MELODY_LEN(MELODY_START));
}

// Check player button press (btn = 1 for S1, 2 for S2, 3 for S3)
void Game_Check_Hit(uint8_t btn) {
    // Mix random seed with human reaction timing
    rand_seed ^= SysTick->VAL;

    if (game_state == STATE_IDLE) {
        if (btn == 1) { // Only S1 starts the game
            Game_Start();
        }
        return;
    }

    if (game_state == STATE_GAMEOVER) {
        if (btn == 1 && gameover_lock_ms == 0) { // Only S1 restarts the game after lock expires
            Game_Start();
        }
        return;
    }

    // During STATE_PLAYING: check if target is visible
    if (current_target == 0) {
        return; // Empty hole (transition pause), ignore press
    }

    if (current_target == 4) {
        // Player hit the BOMB (D4)! Deduct 2 points
        score = (score >= 2) ? (score - 2) : 0;
        Melody_Play(MELODY_BOMB, MELODY_LEN(MELODY_BOMB));
    } else if (btn == current_target) {
        // Player hit the correct MOLE! Add 1 point
        if (score < 99) score++;
        Melody_Play(MELODY_HIT, MELODY_LEN(MELODY_HIT));
    } else {
        // Player pressed the wrong button! Deduct 1 point
        if (score > 0) score--;
        Melody_Play(MELODY_MISS, MELODY_LEN(MELODY_MISS));
    }

    // Hide mole immediately and set short 200 ms pause before next spawn
    current_target = 0;
    target_timer_ms = 200;
}

// Update game timers every 1 ms (called in SysTick_Handler)
void Game_Update_Tick(void) {
    if (gameover_lock_ms > 0) {
        gameover_lock_ms--;
    }

    if (game_state != STATE_PLAYING) {
        return;
    }

    // 1. Update 1-second countdown timer
    sec_tick_ms++;
    if (sec_tick_ms >= 1000) {
        sec_tick_ms = 0;
        if (time_left_sec > 0) {
            time_left_sec--;
        }
        if (time_left_sec == 0) {
            // Time is up -> Game Over!
            game_state = STATE_GAMEOVER;
            current_target = 0;
            gameover_lock_ms = 2600; // Lock buttons for 2.6s to match Game Over melody
            Melody_Play(MELODY_GAMEOVER, MELODY_LEN(MELODY_GAMEOVER));
            return;
        }
    }

    // 2. Update mole/bomb spawn and hide timer
    if (target_timer_ms > 0) {
        target_timer_ms--;
        if (target_timer_ms == 0) {
            if (current_target == 0) {
                // Spawn a new mole or bomb
                current_target = Get_Random_Target();
                target_timer_ms = Get_Mole_Duration_Ms(score);
            } else {
                // Time ran out for current mole/bomb -> hide it for 250 ms
                current_target = 0;
                target_timer_ms = 250;
            }
        }
    }
}

// ============================================================================
// 7. Interrupt Service Routines (ISR)
// ============================================================================

// ISR 1: External Pin Interrupt for buttons S1, S2, S3
void PORTB_IRQHandler(void) {
    uint32_t flags = PORTB->ISFR;

    if (debounce_timer_ms == 0) {
        if (flags & (1U << 3)) {         // Button S1 pressed
            Game_Check_Hit(1);
            debounce_timer_ms = 120;
        } else if (flags & (1U << 10)) { // Button S2 pressed
            Game_Check_Hit(2);
            debounce_timer_ms = 120;
        } else if (flags & (1U << 11)) { // Button S3 pressed
            Game_Check_Hit(3);
            debounce_timer_ms = 120;
        }
    }

    PORTB->ISFR = (1U << 3) | (1U << 10) | (1U << 11);
}

// ISR 2: SysTick Interrupt every 1 ms (Display scan, Game logic, Melody, Debounce)
void SysTick_Handler(void) {
    Display_7Segment_Scan(current_digit_idx);
    current_digit_idx = (current_digit_idx + 1) % 4;

    Game_Update_Tick();
    Melody_Update_Tick();

    if (debounce_timer_ms > 0) {
        debounce_timer_ms--;
    }

    blink_timer_ms++;
    if (blink_timer_ms >= 300) {
        blink_timer_ms = 0;
        blink_state = !blink_state;
    }
}

// ISR 3: FlexTimer 0 Interrupt on overflow (Toggle buzzer pin to create tone)
void FTM0_IRQHandler(void) {
    if (FTM0->SC & FTM_SC_TOF_MASK) {
        FTM0->SC &= ~FTM_SC_TOF_MASK;
    }

    // Toggle PTA1 direction between Output Low (ON) and Input Hi-Z (OFF)
    PTA->PDDR ^= (1U << 1);
}

// ============================================================================
// 8. Main Function
// ============================================================================

int main(void) {
    Init_GPIO_Hardware();
    Init_Switch_Interrupt();
    Init_SysTick_Timer();
    Init_FlexTimer_FTM0();

    while (1) {
        Update_Display_Values(time_left_sec, score);
        Control_Game_LEDs(game_state, current_target, blink_state);
    }
}