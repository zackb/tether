//
//  SharePayload.swift
//  TetherFramework
//
//  Typed representation of content that the Share Extension can send to tetherd.
//

import Foundation

/// The kind of content being shared via the Tether Share Extension.
public enum SharePayload: Sendable {
    /// Send text to the Linux Wayland clipboard (`clipboard_set`).
    case clipboard(String)

    /// Send an OTP/2FA code to the tetherd vault (`new_otp`).
    /// - Parameters:
    ///   - code: The OTP digits or secret string.
    ///   - source: Human-readable label (email subject, website name, etc.).
    case otp(String, source: String)

    /// Send binary data as a file transfer (`file_start` / `file_chunk` / `file_end`).
    case file(Data, filename: String)
}
