/** @file
  Test-only DFCI UI support for unattended QEMU recovery tests.

  Copyright (c) Microsoft Corporation.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/DfciUiSupportLib.h>

BOOLEAN
EFIAPI
DfciUiIsManufacturingMode (
  VOID
  )
{
  return TRUE;
}

BOOLEAN
EFIAPI
DfciUiIsUiAvailable (
  VOID
  )
{
  return TRUE;
}

EFI_STATUS
EFIAPI
DfciUiDisplayMessageBox (
  IN  CHAR16          *TitleBarText,
  IN  CHAR16          *Text,
  IN  CHAR16          *Caption,
  IN  UINT32          Type,
  IN  UINT64          Timeout,
  OUT DFCI_MB_RESULT  *Result
  )
{
  if (Result != NULL) {
    *Result = DFCI_MB_IDOK;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
DfciUiDisplayPasswordDialog (
  IN  CHAR16          *TitleText,
  IN  CHAR16          *CaptionText,
  IN  CHAR16          *BodyText,
  IN  CHAR16          *ErrorText,
  OUT DFCI_MB_RESULT  *Result,
  OUT CHAR16          **Password
  )
{
  return EFI_UNSUPPORTED;
}

EFI_STATUS
EFIAPI
DfciUiDisplayAuthDialog (
  IN  CHAR16          *TitleText,
  IN  CHAR16          *CaptionText,
  IN  CHAR16          *BodyText,
  IN  CHAR16          *CertText,
  IN  CHAR16          *ConfirmText,
  IN  CHAR16          *ErrorText,
  IN  BOOLEAN         PasswordType,
  IN  CHAR16          *Thumbprint,
  OUT DFCI_MB_RESULT  *Result,
  OUT CHAR16          **Password OPTIONAL
  )
{
  if (PasswordType) {
    return EFI_UNSUPPORTED;
  }

  *Result = DFCI_MB_IDOK;
  if (Password != NULL) {
    *Password = NULL;
  }

  return EFI_SUCCESS;
}

VOID
EFIAPI
DfciUiExitSecurityBoundary (
  VOID
  )
{
}
