/// Persistence for the Account tab, so a restart — or an OS kill — does not
/// cost the user their SIP settings.
///
/// Two stores, because the fields are not equally sensitive:
///
///  * Everything non-secret goes into [SharedPreferences] as a single JSON
///    blob. One key means one atomic write and no half-saved profile if the
///    process dies mid-save.
///  * The SIP and TURN passwords go into [FlutterSecureStorage] — Keystore on
///    Android, Keychain on iOS. SharedPreferences is a world-readable XML file
///    on a rooted device and an unencrypted plist on iOS, which is not where
///    live PBX credentials belong.
library;

import 'dart:convert';

import 'package:medut/medut.dart';
import 'package:flutter_secure_storage/flutter_secure_storage.dart';
import 'package:shared_preferences/shared_preferences.dart';

/// The Account tab's form, in a form that survives the process.
class AccountProfile {
  final String uri;
  final String password;
  final Transport transport;
  final String serverUrl;
  final MediaEncryption mediaEnc;
  final bool ice;
  final String stunServer;
  final String turnServer;
  final String turnUser;
  final String turnPass;
  final bool verifyTls;

  /// Codec preference order, including the ones currently switched off — the
  /// order is what the user dragged, and it has to come back the same way.
  final List<String> codecPrefs;

  /// Which of [codecPrefs] are switched on.
  final List<String> enabledCodecs;

  const AccountProfile({
    required this.uri,
    required this.password,
    required this.transport,
    required this.serverUrl,
    required this.mediaEnc,
    required this.ice,
    required this.stunServer,
    required this.turnServer,
    required this.turnUser,
    required this.turnPass,
    required this.verifyTls,
    required this.codecPrefs,
    required this.enabledCodecs,
  });

  /// Everything except the two passwords, which travel via secure storage.
  Map<String, dynamic> _toPrefsJson() => {
        'uri': uri,
        'transport': transport.name,
        'serverUrl': serverUrl,
        'mediaEnc': mediaEnc.name,
        'ice': ice,
        'stunServer': stunServer,
        'turnServer': turnServer,
        'turnUser': turnUser,
        'verifyTls': verifyTls,
        'codecPrefs': codecPrefs,
        'enabledCodecs': enabledCodecs,
      };

  /// Rebuild from stored JSON. Every field falls back to the same default the
  /// form starts with, so a profile written by an older build — one missing a
  /// key added since — still loads instead of throwing on launch.
  static AccountProfile _fromPrefsJson(
    Map<String, dynamic> j,
    String password,
    String turnPass,
  ) {
    List<String> strings(String key, List<String> fallback) {
      final v = j[key];
      return v is List ? v.map((e) => '$e').toList() : fallback;
    }

    T byName<T extends Enum>(String key, List<T> values, T fallback) {
      final v = j[key];
      if (v is! String) return fallback;
      for (final candidate in values) {
        if (candidate.name == v) return candidate;
      }
      return fallback;
    }

    return AccountProfile(
      uri: j['uri'] as String? ?? '',
      password: password,
      transport: byName('transport', Transport.values, Transport.udp),
      serverUrl: j['serverUrl'] as String? ?? '',
      mediaEnc:
          byName('mediaEnc', MediaEncryption.values, MediaEncryption.dtlsSrtp),
      ice: j['ice'] as bool? ?? false,
      stunServer: j['stunServer'] as String? ?? '',
      turnServer: j['turnServer'] as String? ?? '',
      turnUser: j['turnUser'] as String? ?? '',
      turnPass: turnPass,
      verifyTls: j['verifyTls'] as bool? ?? true,
      codecPrefs: strings('codecPrefs', const ['opus', 'ulaw', 'alaw']),
      enabledCodecs: strings('enabledCodecs', const ['opus', 'ulaw']),
    );
  }
}

/// Loads and saves the one [AccountProfile] the example app keeps.
class AccountStore {
  static const _prefsKey = 'medut.account';
  static const _rememberKey = 'medut.account.remember';
  static const _passKey = 'medut.account.password';
  static const _turnPassKey = 'medut.account.turnPassword';

  /// `encryptedSharedPreferences` puts the Android backing store behind
  /// Keystore instead of a plain XML file; on iOS the Keychain item is
  /// `first_unlock` so a background PushKit wake can still read the password
  /// on a locked device.
  static const _secure = FlutterSecureStorage(
    aOptions: AndroidOptions(encryptedSharedPreferences: true),
    iOptions: IOSOptions(accessibility: KeychainAccessibility.first_unlock),
  );

  /// The stored profile, or null when nothing has been saved yet.
  ///
  /// Never throws: a corrupt or unreadable store on a phone the user cannot
  /// debug must not be a crash on launch — it just means an empty form.
  Future<AccountProfile?> load() async {
    try {
      final prefs = await SharedPreferences.getInstance();
      final raw = prefs.getString(_prefsKey);
      if (raw == null) return null;
      final json = jsonDecode(raw);
      if (json is! Map<String, dynamic>) return null;

      // A keystore read can fail on its own — a device restored from backup
      // has the prefs but not the Keystore key. _readSecure falls back to a
      // blank password rather than losing the whole profile with it.
      final password = await _readSecure(_passKey);
      final turnPass = await _readSecure(_turnPassKey);

      return AccountProfile._fromPrefsJson(json, password, turnPass);
    } catch (_) {
      return null;
    }
  }

  /// Write the profile. Passwords go to the keystore; an empty one is deleted
  /// rather than stored, so clearing the field clears the secret too.
  Future<void> save(AccountProfile p) async {
    final prefs = await SharedPreferences.getInstance();
    await prefs.setString(_prefsKey, jsonEncode(p._toPrefsJson()));
    await _writeSecure(_passKey, p.password);
    await _writeSecure(_turnPassKey, p.turnPass);
  }

  /// Whether the user wants the profile kept at all. Defaults to true, which
  /// is what the app does before anyone has touched the switch.
  Future<bool> loadRemember() async {
    try {
      final prefs = await SharedPreferences.getInstance();
      return prefs.getBool(_rememberKey) ?? true;
    } catch (_) {
      return true;
    }
  }

  /// Persist the switch itself — it lives outside the profile blob so that
  /// "off" survives [clear], which wipes the blob.
  Future<void> saveRemember(bool remember) async {
    final prefs = await SharedPreferences.getInstance();
    await prefs.setBool(_rememberKey, remember);
  }

  /// Forget the profile, including the keystore entries. The remember switch
  /// is left alone: it is a preference about future saves, not saved data.
  Future<void> clear() async {
    final prefs = await SharedPreferences.getInstance();
    await prefs.remove(_prefsKey);
    await _deleteSecure(_passKey);
    await _deleteSecure(_turnPassKey);
  }

  static Future<String> _readSecure(String key) async {
    try {
      return await _secure.read(key: key) ?? '';
    } catch (_) {
      return '';
    }
  }

  static Future<void> _writeSecure(String key, String value) async {
    try {
      if (value.isEmpty) {
        await _secure.delete(key: key);
      } else {
        await _secure.write(key: key, value: value);
      }
    } catch (_) {
      // A device with a broken keystore still gets a usable app; the password
      // just has to be typed each launch.
    }
  }

  static Future<void> _deleteSecure(String key) async {
    try {
      await _secure.delete(key: key);
    } catch (_) {}
  }
}
