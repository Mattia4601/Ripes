#pragma once

#include <QDialog>
#include <QStringList>

QT_FORWARD_DECLARE_CLASS(QDialogButtonBox)
QT_FORWARD_DECLARE_CLASS(QLineEdit)
QT_FORWARD_DECLARE_CLASS(QListWidget)

namespace Ripes {

/// Minimal file selector for the remote file system ("remote:/").
///
/// QFileDialog cannot be used on top of the remote file system: its
/// QFileSystemModel queries the file engine from a worker thread and issues a
/// large number of synchronous requests, which blocks the application. This
/// dialog lists the remote directory once, from the main thread, through
/// QDir::entryList().
///
/// Nested exec() calls are not supported on WASM, so the dialog is meant to be
/// shown with open(); the selection is reported through fileSelected().
class RemoteFileDialog : public QDialog {
  Q_OBJECT

public:
  enum class Mode { Open, Save };

  /// \p filter uses the QFileDialog syntax, e.g. "Source files (*.s *.c)";
  /// only the first filter is applied.
  RemoteFileDialog(Mode mode, const QString &title, const QString &filter,
                   QWidget *parent = nullptr);

  void accept() override;

signals:
  /// Emitted on accept with the selected path, e.g. "remote:/program.s".
  void fileSelected(const QString &path);

private:
  void refresh();
  void updateOkButton();

  Mode m_mode;
  QStringList m_nameFilters;
  QStringList m_entries;
  QListWidget *m_fileList = nullptr;
  QLineEdit *m_fileName = nullptr;
  QDialogButtonBox *m_buttons = nullptr;
};

} // namespace Ripes
