#import "RootViewController.h"
#include <errno.h>
#include <signal.h>

// keep in sync with daemon/sink_status.h
#define SINK_DATA_DIR      @"/var/mobile/Library/A2DPSink"
#define SINK_ENABLED_FLAG  SINK_DATA_DIR @"/enabled"
#define SINK_FORGET_FLAG   SINK_DATA_DIR @"/forget"
#define SINK_NAME_FILE     SINK_DATA_DIR @"/name"
#define SINK_STATUS_FILE   SINK_DATA_DIR @"/status"
#define SINK_LAST_DEVICE   SINK_DATA_DIR @"/last_device"
#define SINK_LINK_KEY_DB   SINK_DATA_DIR @"/pairings.tlv"
#define SINK_SETTINGS_FILE SINK_DATA_DIR @"/settings"
#define SINK_MEDIA_GUARD   SINK_DATA_DIR @"/media_guard"
#define SINK_DEFAULT_NAME  @"iPod Receiver"

enum { SectionReceiver, SectionStatus, SectionSettings, SectionCount };
enum { StatusRowState, StatusRowDevice, StatusRowTrack, StatusRowCodec, StatusRowAddress, StatusRowCount };
enum { SettingsRowName, SettingsRowCharging, SettingsRowReconnect, SettingsRowAutoPlay, SettingsRowMediaControls, SettingsRowForget, SettingsRowCount };
enum { AlertRename = 1, AlertForget = 2 };

@interface RootViewController () <UIAlertViewDelegate>
@property (nonatomic, strong) UISwitch * receiverSwitch;
@property (nonatomic, strong) UISwitch * chargingSwitch;
@property (nonatomic, strong) UISwitch * reconnectSwitch;
@property (nonatomic, strong) UISwitch * autoPlaySwitch;
@property (nonatomic, strong) UISwitch * mediaControlsSwitch;
@property (nonatomic, strong) NSDictionary * status;
@property (nonatomic, strong) NSTimer * timer;
@property (nonatomic, strong) NSDate * enabledAt;
@end

@implementation RootViewController

- (instancetype)init {
    return [super initWithStyle:UITableViewStyleGrouped];
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Bluetooth Receiver";
    self.receiverSwitch = [[UISwitch alloc] init];
    [self.receiverSwitch addTarget:self action:@selector(switchChanged:) forControlEvents:UIControlEventValueChanged];
    self.chargingSwitch = [[UISwitch alloc] init];
    [self.chargingSwitch addTarget:self action:@selector(settingSwitchChanged:) forControlEvents:UIControlEventValueChanged];
    self.reconnectSwitch = [[UISwitch alloc] init];
    [self.reconnectSwitch addTarget:self action:@selector(settingSwitchChanged:) forControlEvents:UIControlEventValueChanged];
    self.autoPlaySwitch = [[UISwitch alloc] init];
    [self.autoPlaySwitch addTarget:self action:@selector(settingSwitchChanged:) forControlEvents:UIControlEventValueChanged];
    self.mediaControlsSwitch = [[UISwitch alloc] init];
    [self.mediaControlsSwitch addTarget:self action:@selector(settingSwitchChanged:) forControlEvents:UIControlEventValueChanged];
    [[NSFileManager defaultManager] createDirectoryAtPath:SINK_DATA_DIR withIntermediateDirectories:YES attributes:nil error:NULL];
    [self refresh];
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    self.timer = [NSTimer scheduledTimerWithTimeInterval:1.0 target:self selector:@selector(refresh) userInfo:nil repeats:YES];
}

- (void)viewWillDisappear:(BOOL)animated {
    [super viewWillDisappear:animated];
    [self.timer invalidate];
    self.timer = nil;
}

#pragma mark - state

- (BOOL)enabled {
    return [[NSFileManager defaultManager] fileExistsAtPath:SINK_ENABLED_FLAG];
}

- (NSDictionary *)readStatus {
    NSString * text = [NSString stringWithContentsOfFile:SINK_STATUS_FILE encoding:NSUTF8StringEncoding error:NULL];
    NSMutableDictionary * result = [NSMutableDictionary dictionary];
    for (NSString * line in [text componentsSeparatedByString:@"\n"]) {
        NSRange eq = [line rangeOfString:@"="];
        if (eq.location == NSNotFound) continue;
        result[[line substringToIndex:eq.location]] = [line substringFromIndex:eq.location + 1];
    }
    return result;
}

- (BOOL)daemonAlive {
    int pid = [self.status[@"pid"] intValue];
    if (pid <= 0) return NO;
    // the daemon runs as root: EPERM means it exists
    return kill(pid, 0) == 0 || errno == EPERM;
}

- (NSString *)stateText {
    NSString * state = self.status[@"state"];
    BOOL alive = [self daemonAlive];
    if ([state isEqualToString:@"error"]) return @"Error";
    if (![self enabled]) return alive ? @"Stopping…" : @"Off";
    if (!alive) {
        if (self.enabledAt && [[NSDate date] timeIntervalSinceDate:self.enabledAt] < 15) return @"Starting…";
        // between sessions in "only while charging" mode launchd restarts the daemon
        if ([self onlyWhenCharging]) return @"Waiting for charger";
        return @"Not running";
    }
    if ([state isEqualToString:@"waiting_power"]) return @"Waiting for charger";
    if ([state isEqualToString:@"ready"] && [self.status[@"reconnecting"] length]) {
        return [NSString stringWithFormat:@"Reconnecting to %@…", self.status[@"reconnecting"]];
    }
    if ([state isEqualToString:@"starting"])  return @"Starting…";
    if ([state isEqualToString:@"ready"])     return @"Waiting for a phone";
    if ([state isEqualToString:@"connected"]) return @"Connected";
    if ([state isEqualToString:@"playing"])   return @"Playing";
    if ([state isEqualToString:@"stopping"])  return @"Stopping…";
    return state.length ? state : @"—";
}

- (NSString *)nonEmpty:(NSString *)s {
    return s.length ? s : @"—";
}

- (NSString *)configuredName {
    NSString * name = [NSString stringWithContentsOfFile:SINK_NAME_FILE encoding:NSUTF8StringEncoding error:NULL];
    name = [name stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
    return name.length ? name : SINK_DEFAULT_NAME;
}

// settings file: key=value lines, read by the daemon every second
- (NSDictionary *)readSettings {
    NSString * text = [NSString stringWithContentsOfFile:SINK_SETTINGS_FILE encoding:NSUTF8StringEncoding error:NULL];
    NSMutableDictionary * result = [NSMutableDictionary dictionaryWithDictionary:@{ @"only_when_charging": @"0", @"auto_reconnect": @"1", @"auto_play": @"0", @"media_controls": @"1" }];
    for (NSString * line in [text componentsSeparatedByString:@"\n"]) {
        NSRange eq = [line rangeOfString:@"="];
        if (eq.location == NSNotFound) continue;
        result[[line substringToIndex:eq.location]] = [line substringFromIndex:eq.location + 1];
    }
    return result;
}

- (BOOL)onlyWhenCharging {
    return [[self readSettings][@"only_when_charging"] intValue] != 0;
}

- (BOOL)autoReconnect {
    return [[self readSettings][@"auto_reconnect"] intValue] != 0;
}

- (BOOL)autoPlay {
    return [[self readSettings][@"auto_play"] intValue] != 0;
}

- (BOOL)mediaControls {
    return [[self readSettings][@"media_controls"] intValue] != 0;
}

- (void)writeSettings {
    NSString * text = [NSString stringWithFormat:@"only_when_charging=%d\nauto_reconnect=%d\nauto_play=%d\nmedia_controls=%d\n",
                       self.chargingSwitch.on ? 1 : 0, self.reconnectSwitch.on ? 1 : 0, self.autoPlaySwitch.on ? 1 : 0, self.mediaControlsSwitch.on ? 1 : 0];
    [text writeToFile:SINK_SETTINGS_FILE atomically:YES encoding:NSUTF8StringEncoding error:NULL];
}

- (void)refresh {
    self.status = [self readStatus];
    if (!self.receiverSwitch.isTracking) {
        [self.receiverSwitch setOn:[self enabled] animated:YES];
    }
    if (!self.chargingSwitch.isTracking)  [self.chargingSwitch setOn:[self onlyWhenCharging] animated:NO];
    if (!self.reconnectSwitch.isTracking) [self.reconnectSwitch setOn:[self autoReconnect] animated:NO];
    if (!self.autoPlaySwitch.isTracking)  [self.autoPlaySwitch setOn:[self autoPlay] animated:NO];
    if (!self.mediaControlsSwitch.isTracking) [self.mediaControlsSwitch setOn:[self mediaControls] animated:NO];
    [self.tableView reloadData];
}

#pragma mark - actions

- (void)switchChanged:(UISwitch *)sender {
    NSFileManager * fm = [NSFileManager defaultManager];
    if (sender.on) {
        self.enabledAt = [NSDate date];
        [fm createFileAtPath:SINK_ENABLED_FLAG contents:[NSData data] attributes:nil];
    } else {
        self.enabledAt = nil;
        [fm removeItemAtPath:SINK_ENABLED_FLAG error:NULL];
    }
    [self refresh];
}

- (void)settingSwitchChanged:(UISwitch *)sender {
    if (sender == self.mediaControlsSwitch) {
        // let the daemon retry media controls after an automatic shut-off
        [[NSFileManager defaultManager] removeItemAtPath:SINK_MEDIA_GUARD error:NULL];
    }
    [self writeSettings];
    [self refresh];
}

- (void)renameTapped {
    UIAlertView * alert = [[UIAlertView alloc] initWithTitle:@"Bluetooth Name"
                                                     message:@"Phones will see the receiver under this name. Applies the next time the receiver is switched on."
                                                    delegate:self cancelButtonTitle:@"Cancel" otherButtonTitles:@"Save", nil];
    alert.tag = AlertRename;
    alert.alertViewStyle = UIAlertViewStylePlainTextInput;
    [alert textFieldAtIndex:0].text = [self configuredName];
    [alert show];
}

- (void)forgetTapped {
    UIAlertView * alert = [[UIAlertView alloc] initWithTitle:@"Forget Paired Devices?"
                                                     message:@"Phones will have to pair again. Also remove the receiver from the phone's Bluetooth settings."
                                                    delegate:self cancelButtonTitle:@"Cancel" otherButtonTitles:@"Forget", nil];
    alert.tag = AlertForget;
    [alert show];
}

- (void)alertView:(UIAlertView *)alertView clickedButtonAtIndex:(NSInteger)buttonIndex {
    if (buttonIndex == alertView.cancelButtonIndex) return;
    NSFileManager * fm = [NSFileManager defaultManager];
    if (alertView.tag == AlertRename) {
        NSString * name = [[alertView textFieldAtIndex:0].text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
        if (name.length > 48) name = [name substringToIndex:48];
        if (name.length) {
            [[name stringByAppendingString:@"\n"] writeToFile:SINK_NAME_FILE atomically:YES encoding:NSUTF8StringEncoding error:NULL];
        } else {
            [fm removeItemAtPath:SINK_NAME_FILE error:NULL];
        }
    } else if (alertView.tag == AlertForget) {
        if ([self daemonAlive]) {
            [fm createFileAtPath:SINK_FORGET_FLAG contents:[NSData data] attributes:nil];
        } else {
            [fm removeItemAtPath:SINK_LINK_KEY_DB error:NULL];
            [fm removeItemAtPath:SINK_LAST_DEVICE error:NULL];
        }
    }
    [self refresh];
}

#pragma mark - table

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView {
    return SectionCount;
}

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    switch (section) {
        case SectionReceiver: return 1;
        case SectionStatus:   return StatusRowCount;
        case SectionSettings: return SettingsRowCount;
    }
    return 0;
}

- (NSString *)tableView:(UITableView *)tableView titleForHeaderInSection:(NSInteger)section {
    if (section == SectionStatus) return @"Status";
    return nil;
}

- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    if (section == SectionReceiver) {
        if ([self.status[@"state"] isEqualToString:@"error"] && [self.status[@"error"] length]) {
            return [NSString stringWithFormat:@"%@. Details: /var/log/a2dpsinkd.log", self.status[@"error"]];
        }
        return [NSString stringWithFormat:@"Pair your phone with \"%@\". While the receiver is on, iOS Bluetooth is off and the iPod stays awake.",
                [self configuredName]];
    }
    if (section == SectionSettings) {
        NSString * media = self.status[@"media"];
        NSString * note = @"";
        if ([media isEqualToString:@"failed"]) {
            note = @"\u26A0\uFE0F Lock Screen & Car Controls crashed on this iPod and were turned off automatically; the receiver itself keeps working. Switch them off and on to try again.\n\n";
        } else if ([media isEqualToString:@"unavailable"]) {
            note = @"\u26A0\uFE0F Lock Screen & Car Controls aren't available on this iOS version; the receiver itself keeps working.\n\n";
        }
        return [note stringByAppendingString:@"Only While Charging: the receiver turns on when the iPod is connected to power (charger, computer or dock) and hands Bluetooth back to iOS 5 seconds after it is unplugged.\n\nReconnect Automatically: connects back to the last phone when the receiver starts and after the connection was lost.\n\nAuto Play on Connect: tells the phone to start playing as soon as it connects (if it isn't playing already).\n\nLock Screen & Car Controls: shows the track on the lock screen and lets the car head unit's play/pause and next/previous buttons control the phone over Bluetooth, instead of the iPod's own Music app."];
    }
    return nil;
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    static NSString * identifier = @"Cell";
    UITableViewCell * cell = [tableView dequeueReusableCellWithIdentifier:identifier];
    if (!cell) {
        cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleValue1 reuseIdentifier:identifier];
    }
    cell.accessoryView = nil;
    cell.accessoryType = UITableViewCellAccessoryNone;
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
    cell.textLabel.textColor = [UIColor blackColor];
    cell.textLabel.textAlignment = NSTextAlignmentLeft;
    cell.detailTextLabel.text = nil;

    if (indexPath.section == SectionReceiver) {
        cell.textLabel.text = @"Receiver";
        cell.accessoryView = self.receiverSwitch;
    } else if (indexPath.section == SectionStatus) {
        BOOL connected = [self daemonAlive] && [self enabled];
        switch (indexPath.row) {
            case StatusRowState:
                cell.textLabel.text = @"State";
                cell.detailTextLabel.text = [self stateText];
                break;
            case StatusRowDevice: {
                cell.textLabel.text = @"Device";
                NSString * name = self.status[@"remote_name"];
                cell.detailTextLabel.text = connected ? [self nonEmpty:(name.length ? name : self.status[@"remote_address"])] : @"—";
                break;
            }
            case StatusRowTrack: {
                cell.textLabel.text = @"Now Playing";
                NSString * title = self.status[@"title"], * artist = self.status[@"artist"];
                NSString * text = title.length && artist.length ? [NSString stringWithFormat:@"%@ – %@", title, artist] : (title.length ? title : artist);
                cell.detailTextLabel.text = connected ? [self nonEmpty:text] : @"—";
                break;
            }
            case StatusRowCodec:
                cell.textLabel.text = @"Audio";
                cell.detailTextLabel.text = connected ? [self nonEmpty:self.status[@"codec"]] : @"—";
                break;
            case StatusRowAddress:
                cell.textLabel.text = @"Address";
                cell.detailTextLabel.text = [self nonEmpty:self.status[@"address"]];
                break;
        }
    } else if (indexPath.section == SectionSettings) {
        cell.selectionStyle = UITableViewCellSelectionStyleBlue;
        if (indexPath.row == SettingsRowName) {
            cell.textLabel.text = @"Name";
            cell.detailTextLabel.text = [self configuredName];
            cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
        } else if (indexPath.row == SettingsRowCharging) {
            cell.textLabel.text = @"Only While Charging";
            cell.accessoryView = self.chargingSwitch;
            cell.selectionStyle = UITableViewCellSelectionStyleNone;
        } else if (indexPath.row == SettingsRowReconnect) {
            cell.textLabel.text = @"Reconnect Automatically";
            cell.accessoryView = self.reconnectSwitch;
            cell.selectionStyle = UITableViewCellSelectionStyleNone;
        } else if (indexPath.row == SettingsRowAutoPlay) {
            cell.textLabel.text = @"Auto Play on Connect";
            cell.accessoryView = self.autoPlaySwitch;
            cell.selectionStyle = UITableViewCellSelectionStyleNone;
        } else if (indexPath.row == SettingsRowMediaControls) {
            cell.textLabel.text = @"Lock Screen & Car Controls";
            cell.accessoryView = self.mediaControlsSwitch;
            cell.selectionStyle = UITableViewCellSelectionStyleNone;
        } else {
            cell.textLabel.text = @"Forget Paired Devices";
            cell.textLabel.textColor = [UIColor redColor];
        }
    }
    return cell;
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    if (indexPath.section != SectionSettings) return;
    if (indexPath.row == SettingsRowName) [self renameTapped];
    else if (indexPath.row == SettingsRowForget) [self forgetTapped];
}

@end
