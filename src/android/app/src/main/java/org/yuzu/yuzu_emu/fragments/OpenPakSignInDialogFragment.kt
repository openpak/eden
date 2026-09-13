// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.fragments

import android.app.Dialog
import android.os.Bundle
import android.text.InputType
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import androidx.fragment.app.DialogFragment
import androidx.lifecycle.lifecycleScope
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import kotlinx.coroutines.launch
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.utils.OpenPak

/**
 * Signing this install in to OpenPak.
 *
 * Signed in is not linked: the emulator gets a perfectly valid token on its own, and a title
 * server can attach it to nobody until this runs -- which is a game that loads, reaches the
 * network, and then cannot find a friend. The console asks the same thing with an on-screen
 * keyboard; a phone already has one, so this is the two fields and nothing else.
 */
class OpenPakSignInDialogFragment : DialogFragment() {
    override fun onCreateDialog(savedInstanceState: Bundle?): Dialog {
        val padding = (24 * resources.displayMetrics.density).toInt()

        val status = TextView(requireContext())
        val email = EditText(requireContext()).apply {
            hint = getString(R.string.openpak_email)
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_EMAIL_ADDRESS
        }
        val password = EditText(requireContext()).apply {
            hint = getString(R.string.openpak_password)
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD
        }

        val layout = LinearLayout(requireContext()).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(padding, padding / 2, padding, 0)
            addView(status)
            addView(email)
            addView(password)
        }

        // What the last sign-in left behind, so somebody already linked is told so rather than
        // being asked again.
        lifecycleScope.launch {
            val current = OpenPak.status()
            status.text = when {
                !current.enabled -> getString(R.string.openpak_no_server)
                current.linked -> getString(
                    R.string.openpak_linked_as,
                    current.nickname,
                    current.friendCode
                )
                else -> getString(R.string.openpak_not_linked)
            }
        }

        return MaterialAlertDialogBuilder(requireContext())
            .setTitle(R.string.openpak_account)
            .setView(layout)
            .setPositiveButton(R.string.openpak_sign_in) { _, _ ->
                lifecycleScope.launch {
                    val failure = OpenPak.signIn(
                        email.text.toString().trim(),
                        password.text.toString()
                    )

                    val message = if (failure.isEmpty()) {
                        val now = OpenPak.status()
                        getString(R.string.openpak_linked_as, now.nickname, now.friendCode)
                    } else {
                        failure
                    }

                    Toast.makeText(requireContext(), message, Toast.LENGTH_LONG).show()
                }
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    companion object {
        const val TAG = "OpenPakSignInDialogFragment"
    }
}
